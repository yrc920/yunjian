#include "StatusServiceImpl.h"
#include "ChatControlClient.h"
#include "ConfigMgr.h"
#include "InstanceRegistry.h"
#include "Protocol.h"
#include "RedisMgr.h"
#include "SessionRegistry.h"
#include "const.h"
#include <boost/uuid/uuid_io.hpp>
#include <algorithm>
#include <iostream>
#include <set>
#include "Log.h"

namespace {

constexpr int KICK_REASON_SUPERSEDED = 1;

std::string GenerateToken()
{
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	return to_string(uuid);
}

} // namespace

StatusServiceImpl::StatusServiceImpl()
{
	auto& cfg = ConfigMgr::Inst();
	//资源服务器地址与聊天实例一起下发，客户端登录后用它上传头像、拉取缺失资源
	_res_host = cfg["ResourceServer"]["Host"];
	_res_port = cfg["ResourceServer"]["Port"];

	//实例白名单在 InstanceRegistry 构造时读取，这里提前触发一次，配置错误在启动日志里就能看到
	InstanceRegistry::Inst();
}

bool StatusServiceImpl::VerifyToken(int uid, const std::string& token)
{
	if (uid <= 0 || token.empty())
		return false;
	std::string stored;
	if (!RedisMgr::GetInstance()->Get(USERTOKENPREFIX + std::to_string(uid), stored))
		return false;
	return stored == token;
}

bool StatusServiceImpl::IssueToken(int uid, std::string& token)
{
	token = GenerateToken();
	//令牌带有效期，泄露后不会永久可用；每次登录刷新，长期不登录的会话自然失效
	return RedisMgr::GetInstance()->SetEx(USERTOKENPREFIX + std::to_string(uid),
		token, kTokenTtlSeconds);
}

//登录选路：实例必须存活、未下线、地址完整，负载按 sessions/capacity 计算
Status StatusServiceImpl::GetChatServer(ServerContext* context,
	const GetChatServerReq* request, GetChatServerRsp* reply)
{
	if (request->uid() <= 0) {
		reply->set_error(ErrorCodes::UidInvalid);
		return Status::OK;
	}

	InstanceInfo instance;
	if (!InstanceRegistry::Inst().Pick(instance)) {
		LOG_WARN << "[session] route failed uid=" << request->uid() << " no_instance";
		reply->set_error(ErrorCodes::ServiceUnavailable);
		return Status::OK;
	}

	std::string token;
	if (!IssueToken(request->uid(), token)) {
		reply->set_error(ErrorCodes::ServiceUnavailable);
		return Status::OK;
	}

	reply->set_host(instance.host);
	reply->set_port(std::to_string(instance.client_port));
	reply->set_token(token);
	reply->set_res_host(_res_host);
	reply->set_res_port(_res_port);
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::AllocateSession(ServerContext* context,
	const AllocateSessionReq* request, AllocateSessionRsp* reply)
{
	const int uid = request->uid();
	if (uid <= 0) {
		reply->set_error(ErrorCodes::UidInvalid);
		return Status::OK;
	}
	if (request->token().empty() || request->session_id().empty() || request->instance().empty()) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}

	//先校验令牌再动租约：失败的登录不能产生任何状态变更，也不能把旧设备踢掉
	if (!VerifyToken(uid, request->token())) {
		reply->set_error(ErrorCodes::TokenInvalid);
		return Status::OK;
	}

	InstanceInfo self;
	if (!InstanceRegistry::Inst().Get(request->instance(), self)) {
		reply->set_error(ErrorCodes::InstanceUnknown);
		return Status::OK;
	}

	long long epoch = 0;
	std::string kicked_instance;
	std::string kicked_session_id;
	bool rate_limited = false;
	if (!SessionRegistry::Inst().Allocate(uid, request->session_id(), request->instance(),
		epoch, kicked_instance, kicked_session_id, rate_limited)) {
		reply->set_error(rate_limited ? ErrorCodes::TooManyRequests : ErrorCodes::ServiceUnavailable);
		return Status::OK;
	}

	reply->set_epoch(epoch);
	reply->set_lease_seconds(LEASE_TTL_SECONDS);
	reply->set_error(ErrorCodes::Success);

	//先把新租约写进去，再异步通知旧实例：即使通知丢失，旧连接也会在下一次续租被 Fenced
	if (!kicked_instance.empty()) {
		reply->set_kicked_instance(kicked_instance);
		reply->set_kicked_uid(uid);
		InstanceInfo old;
		if (InstanceRegistry::Inst().Get(kicked_instance, old)) {
			ChatControlClient::Inst().KickAsync(old.rpc_host, old.rpc_port, uid, epoch,
				kicked_session_id, KICK_REASON_SUPERSEDED);
		}
	}

	LOG_INFO << "[session] alloc uid=" << uid << " epoch=" << epoch
		<< " inst=" << request->instance() << " kicked=" << kicked_instance;
	return Status::OK;
}

Status StatusServiceImpl::RenewSessions(ServerContext* context,
	const RenewSessionsReq* request, RenewSessionsRsp* reply)
{
	if (request->instance().empty() || request->items_size() > 2000) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}
	//心跳与续租合并：一次 RPC 既刷新实例存活，也刷新会话 TTL
	bool draining = false;
	if (!InstanceRegistry::Inst().Heartbeat(request->instance(), request->conn_count(),
		request->session_count(), draining)) {
		reply->set_error(ErrorCodes::InstanceUnknown);
		return Status::OK;
	}
	reply->set_draining(draining);

	for (const auto& item : request->items()) {
		auto* result = reply->add_items();
		result->set_uid(item.uid());
		result->set_session_id(item.session_id());
		long long epoch = 0;
		const int status = SessionRegistry::Inst().Renew(item.uid(), item.session_id(),
			request->instance(), epoch);
		result->set_status(status);
		result->set_epoch(epoch);
	}
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::ReleaseSession(ServerContext* context,
	const ReleaseSessionReq* request, ReleaseSessionRsp* reply)
{
	if (request->uid() <= 0 || request->session_id().empty()) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}

	bool released = false;
	if (!SessionRegistry::Inst().Release(request->uid(), request->session_id(),
		request->epoch(), released)) {
		reply->set_error(ErrorCodes::ServiceUnavailable);
		return Status::OK;
	}

	//被顶替或已经过期都属于正常结果：连接已经关闭，租约本来就不归它了
	reply->set_released(released);
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::LogoutSession(ServerContext* context,
	const LogoutSessionReq* request, LogoutSessionRsp* reply)
{
	if (request->uid() <= 0 || request->session_id().empty() || request->epoch() <= 0) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}

	bool revoked = false;
	if (!SessionRegistry::Inst().Logout(request->uid(), request->session_id(),
		request->epoch(), revoked)) {
		reply->set_error(ErrorCodes::ServiceUnavailable);
		return Status::OK;
	}

	LOG_INFO << "[session] logout uid=" << request->uid()
		<< " revoked=" << revoked;
	reply->set_revoked(revoked);
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::LookupRoute(ServerContext* context,
	const LookupRouteReq* request, LookupRouteRsp* reply)
{
	if (request->uid() <= 0) {
		reply->set_error(ErrorCodes::UidInvalid);
		return Status::OK;
	}

	SessionLease lease;
	if (SessionRegistry::Inst().Lookup(request->uid(), lease)) {
		reply->set_online(true);
		reply->set_instance(lease.instance);
		reply->set_epoch(lease.epoch);
	}
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::BatchLookupRoute(ServerContext* context,
	const BatchLookupRouteReq* request, BatchLookupRouteRsp* reply)
{
	if (request->uids_size() == 0) {
		reply->set_error(ErrorCodes::Success);
		return Status::OK;
	}

	//按脚本上限分批，避免一次把过多 key 塞进 Lua
	std::vector<int> uids(request->uids().begin(), request->uids().end());
	std::map<int, SessionLease> found;
	for (size_t offset = 0; offset < uids.size(); offset += MAX_BATCH_LOOKUP) {
		const size_t end = std::min(uids.size(), offset + MAX_BATCH_LOOKUP);
		std::vector<int> chunk(uids.begin() + offset, uids.begin() + end);
		auto part = SessionRegistry::Inst().BatchLookup(chunk);
		found.insert(part.begin(), part.end());
	}

	//每个请求的 uid 都要有回包，离线用 online=false 表示，调用方不用靠"缺失"推断
	std::set<int> seen;
	for (int uid : uids) {
		if (uid <= 0 || !seen.insert(uid).second)
			continue;
		auto* entry = reply->add_entries();
		entry->set_uid(uid);
		auto iter = found.find(uid);
		if (iter != found.end()) {
			entry->set_online(true);
			entry->set_instance(iter->second.instance);
			entry->set_epoch(iter->second.epoch);
		}
	}
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::RegisterInstance(ServerContext* context,
	const RegisterInstanceReq* request, RegisterInstanceRsp* reply)
{
	if (request->name().empty() || request->host().empty() ||
		request->client_port() <= 0 || request->rpc_port() <= 0) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}

	int heartbeat = INSTANCE_HEARTBEAT_SECONDS;
	bool draining = false;
	if (!InstanceRegistry::Inst().Register(request->name(), request->host(), request->rpc_host(),
		request->client_port(), request->rpc_port(), request->capacity(), heartbeat, draining)) {
		//白名单外的注册请求按未授权处理，不暴露"配置里有哪些实例"
		reply->set_error(ErrorCodes::InstanceUnknown);
		return Status::OK;
	}

	reply->set_heartbeat_seconds(heartbeat);
	reply->set_draining(draining);
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::UnregisterInstance(ServerContext* context,
	const UnregisterInstanceReq* request, UnregisterInstanceRsp* reply)
{
	if (request->name().empty()) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}
	InstanceRegistry::Inst().Unregister(request->name());
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}

Status StatusServiceImpl::DrainInstance(ServerContext* context,
	const DrainInstanceReq* request, DrainInstanceRsp* reply)
{
	if (request->name().empty()) {
		reply->set_error(ErrorCodes::Error_Json);
		return Status::OK;
	}
	//宽限期限制在 0~3600 秒，防止一次误调用让实例永远不再接流量
	const int grace = std::max(0, std::min(request->grace_seconds(), 3600));
	if (!InstanceRegistry::Inst().Drain(request->name(), grace)) {
		reply->set_error(ErrorCodes::InstanceUnknown);
		return Status::OK;
	}
	reply->set_error(ErrorCodes::Success);
	return Status::OK;
}
