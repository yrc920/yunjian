#include "StatusGrpcClient.h"
#include "ConfigMgr.h"
#include <chrono>
#include <iostream>
#include "Log.h"

namespace {

void SetDeadline(grpc::ClientContext& context, int milliseconds)
{
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(milliseconds));
}

} // namespace

StatusGrpcClient::StatusGrpcClient()
{
	auto& cfg = ConfigMgr::Inst();
	pool_.reset(new StatusConPool(5, cfg["StatusServer"]["Host"], cfg["StatusServer"]["Port"]));
	_release_thread = std::thread([this]() { ReleaseLoop(); });
}

StatusGrpcClient::~StatusGrpcClient()
{
	Shutdown();
	pool_->Close();
}

void StatusGrpcClient::Shutdown()
{
	if (_stop.exchange(true))
		return;
	_queue_cv.notify_all();
	if (_release_thread.joinable())
		_release_thread.join();
}

void StatusGrpcClient::ReleaseAsync(int uid, const std::string& session_id, long long epoch)
{
	if (uid <= 0 || session_id.empty())
		return;
	{
		std::lock_guard<std::mutex> guard(_queue_mutex);
		_release_tasks.push_back(ReleaseTask{ uid, session_id, epoch });
	}
	_queue_cv.notify_one();
}

void StatusGrpcClient::ReleaseLoop()
{
	//停服时最多排空 3 秒：状态服务不可达时不能让进程退出被成百上千条释放请求拖住，
	//没发出去的租约由 90 秒 TTL 兜底
	std::chrono::steady_clock::time_point drain_deadline;
	bool draining = false;

	while (true) {
		ReleaseTask task;
		{
			std::unique_lock<std::mutex> lock(_queue_mutex);
			_queue_cv.wait(lock, [this]() { return _stop.load() || !_release_tasks.empty(); });
			if (_stop.load() && !draining) {
				draining = true;
				drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			}
			if (draining && std::chrono::steady_clock::now() >= drain_deadline) {
				_release_tasks.clear();
				return;
			}
			if (_release_tasks.empty()) {
				if (_stop.load())
					return;
				continue;
			}
			task = _release_tasks.front();
			_release_tasks.pop_front();
		}
		ReleaseSession(task.uid, task.session_id, task.epoch);
	}
}

message::AllocateSessionRsp StatusGrpcClient::AllocateSession(int uid, const std::string& token,
	const std::string& session_id, const std::string& instance)
{
	message::AllocateSessionRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(1500);
	if (!stub)
		return reply;

	message::AllocateSessionReq request;
	request.set_uid(uid);
	request.set_token(token);
	request.set_session_id(session_id);
	request.set_instance(instance);

	grpc::ClientContext context;
	//登录链路的等待上限：状态服务不可用时快速失败，客户端可以马上重试
	SetDeadline(context, 2000);
	const grpc::Status status = stub->AllocateSession(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
		LOG_ERROR << "[login] allocate rpc failed uid=" << uid
			<< " err=" << status.error_message();
	}
	return reply;
}

message::RenewSessionsRsp StatusGrpcClient::RenewSessions(const std::string& instance,
	const std::vector<message::SessionLeaseRef>& leases, int conn_count, int session_count)
{
	message::RenewSessionsRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(1000);
	if (!stub)
		return reply;

	message::RenewSessionsReq request;
	request.set_instance(instance);
	request.set_conn_count(conn_count);
	request.set_session_count(session_count);
	for (const auto& lease : leases)
		*request.add_items() = lease;

	grpc::ClientContext context;
	SetDeadline(context, 1500);
	const grpc::Status status = stub->RenewSessions(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
		LOG_WARN << "[lease] renew rpc failed inst=" << instance
			<< " err=" << status.error_message();
	}
	return reply;
}

message::ReleaseSessionRsp StatusGrpcClient::ReleaseSession(int uid,
	const std::string& session_id, long long epoch)
{
	message::ReleaseSessionRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(500);
	if (!stub)
		return reply;

	message::ReleaseSessionReq request;
	request.set_uid(uid);
	request.set_session_id(session_id);
	request.set_epoch(epoch);

	grpc::ClientContext context;
	SetDeadline(context, 500);
	const grpc::Status status = stub->ReleaseSession(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
		return reply;
	}
	//被顶替或已经过期都不算错误，日志里保留区分便于排查"为什么没下线"
	if (!reply.released()) {
		LOG_DEBUG << "[lease] release ignored uid=" << uid << " epoch=" << epoch;
	}
	return reply;
}

message::LogoutSessionRsp StatusGrpcClient::LogoutSession(int uid,
	const std::string& session_id, long long epoch)
{
	message::LogoutSessionRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);
	reply.set_revoked(false);

	auto stub = pool_->getConnection(500);
	if (!stub)
		return reply;

	message::LogoutSessionReq request;
	request.set_uid(uid);
	request.set_session_id(session_id);
	request.set_epoch(epoch);

	grpc::ClientContext context;
	//登出在请求路径上，最多等 800ms；失败时本地照常下线，令牌由 TTL 兜底
	SetDeadline(context, 800);
	const grpc::Status status = stub->LogoutSession(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
		reply.set_revoked(false);
	}
	return reply;
}

message::LookupRouteRsp StatusGrpcClient::LookupRoute(int uid)
{
	message::LookupRouteRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(300);
	if (!stub)
		return reply;

	message::LookupRouteReq request;
	request.set_uid(uid);

	grpc::ClientContext context;
	SetDeadline(context, 800);
	const grpc::Status status = stub->LookupRoute(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
	}
	return reply;
}

message::BatchLookupRouteRsp StatusGrpcClient::BatchLookupRoute(const std::vector<int>& uids)
{
	message::BatchLookupRouteRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);
	if (uids.empty())
		return reply;

	auto stub = pool_->getConnection(500);
	if (!stub)
		return reply;

	message::BatchLookupRouteReq request;
	for (int uid : uids)
		request.add_uids(uid);

	grpc::ClientContext context;
	SetDeadline(context, 1000);
	const grpc::Status status = stub->BatchLookupRoute(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
	}
	return reply;
}

message::RegisterInstanceRsp StatusGrpcClient::RegisterInstance(const std::string& name,
	const std::string& host, const std::string& rpc_host, int client_port, int rpc_port, int capacity)
{
	message::RegisterInstanceRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(1000);
	if (!stub)
		return reply;

	message::RegisterInstanceReq request;
	request.set_name(name);
	request.set_host(host);
	request.set_rpc_host(rpc_host);
	request.set_client_port(client_port);
	request.set_rpc_port(rpc_port);
	request.set_capacity(capacity);

	grpc::ClientContext context;
	SetDeadline(context, 1000);
	const grpc::Status status = stub->RegisterInstance(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
	}
	return reply;
}

message::UnregisterInstanceRsp StatusGrpcClient::UnregisterInstance(const std::string& name)
{
	message::UnregisterInstanceRsp reply;
	reply.set_error(ErrorCodes::ServiceUnavailable);

	auto stub = pool_->getConnection(500);
	if (!stub)
		return reply;

	message::UnregisterInstanceReq request;
	request.set_name(name);

	grpc::ClientContext context;
	SetDeadline(context, 500);
	const grpc::Status status = stub->UnregisterInstance(&context, request, &reply);
	pool_->returnConnection(std::move(stub));

	if (!status.ok()) {
		reply.set_error(ErrorCodes::ServiceUnavailable);
	}
	return reply;
}

StatusConPool::StatusConPool(size_t poolSize, const std::string& host, const std::string& port)
	: poolSize_(poolSize), host_(host), port_(port), b_stop_(false)
{
	for (size_t i = 0; i < poolSize_; ++i) {
		auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
		connections_.push(message::StatusService::NewStub(channel));
	}
}

std::unique_ptr<message::StatusService::Stub> StatusConPool::getConnection(int timeout_ms)
{
	std::unique_lock<std::mutex> lock(mutex_);
	//有界等待：状态服务变慢时不能把调用线程永久挂住
	const bool ready = cond_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
		return b_stop_.load() || !connections_.empty();
	});
	if (b_stop_ || !ready)
		return nullptr;

	auto context = std::move(connections_.front());
	connections_.pop();
	return context;
}

void StatusConPool::returnConnection(std::unique_ptr<message::StatusService::Stub> context)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (b_stop_)
		return;
	connections_.push(std::move(context));
	cond_.notify_one();
}

void StatusConPool::Close()
{
	b_stop_ = true;
	cond_.notify_all();
}

StatusConPool::~StatusConPool()
{
	Close();
	std::lock_guard<std::mutex> lock(mutex_);
	while (!connections_.empty())
		connections_.pop();
}
