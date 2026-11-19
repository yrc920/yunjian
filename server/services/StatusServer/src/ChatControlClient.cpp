#include "ChatControlClient.h"
#include <chrono>
#include <iostream>
#include "Log.h"

ChatControlClient& ChatControlClient::Inst()
{
	static ChatControlClient client;
	return client;
}

ChatControlClient::ChatControlClient()
{
	_worker = std::thread([this]() { WorkerLoop(); });
}

ChatControlClient::~ChatControlClient()
{
	Shutdown();
}

void ChatControlClient::Shutdown()
{
	if (_stop.exchange(true))
		return;
	_queue_cv.notify_all();
	if (_worker.joinable())
		_worker.join();
}

std::shared_ptr<message::ChatService::Stub> ChatControlClient::StubFor(const std::string& address)
{
	std::lock_guard<std::mutex> guard(_stub_mutex);
	auto iter = _stubs.find(address);
	if (iter != _stubs.end())
		return iter->second;

	//同一地址复用一个通道；gRPC 的 Stub 本身线程安全，不需要连接池
	auto channel = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
	auto stub = std::shared_ptr<message::ChatService::Stub>(
		message::ChatService::NewStub(channel));
	_stubs[address] = stub;
	return stub;
}

void ChatControlClient::KickAsync(const std::string& host, int port, int uid,
	long long epoch, const std::string& session_id, int reason)
{
	if (host.empty() || port <= 0 || uid <= 0 || session_id.empty())
		return;
	{
		std::lock_guard<std::mutex> guard(_queue_mutex);
		_tasks.push_back(Task{ host, port, uid, epoch, session_id, reason });
	}
	_queue_cv.notify_one();
}

void ChatControlClient::WorkerLoop()
{
	while (true) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(_queue_mutex);
			_queue_cv.wait(lock, [this]() { return _stop.load() || !_tasks.empty(); });
			//停止时先把已排队的任务处理完，避免登出/顶号通知被截断
			if (_tasks.empty()) {
				if (_stop.load())
					return;
				continue;
			}
			task = _tasks.front();
			_tasks.pop_front();
		}
		Kick(task.host, task.port, task.uid, task.epoch, task.session_id, task.reason);
	}
}

bool ChatControlClient::Kick(const std::string& host, int port, int uid,
	long long epoch, const std::string& session_id, int reason)
{
	const std::string address = host + ":" + std::to_string(port);
	auto stub = StubFor(address);
	if (!stub)
		return false;

	message::KickSessionReq request;
	request.set_uid(uid);
	request.set_epoch(epoch);
	request.set_session_id(session_id);
	request.set_reason(reason);

	message::KickSessionRsp response;
	grpc::ClientContext context;
	//踢人是尽力通知，不能因为对端卡住拖慢后台任务
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(800));

	const grpc::Status status = stub->KickSession(&context, request, &response);
	if (!status.ok() || response.error() != 0) {
		LOG_WARN << "[session] kick failed uid=" << uid << " epoch=" << epoch
			<< " target=" << address << " err=" << status.error_message();
		return false;
	}
	LOG_INFO << "[session] kick uid=" << uid << " epoch=" << epoch
		<< " target=" << address << " closed=" << response.closed();
	return true;
}
