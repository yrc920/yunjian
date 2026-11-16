#include "InstanceRegistry.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <iostream>

namespace {

//注册：保留已有的负载与 draining 状态，只覆盖地址与容量
//这样进程重启不会把运维设置的"下线中"抹掉，负载也不会瞬间归零后抢到大量新连接
const char* kRegisterScript = R"lua(
if redis.call('EXISTS', KEYS[1]) == 0 then
  redis.call('HSET', KEYS[1], 'draining', 0)
end
--注册意味着进程刚启动，负载从 0 重新开始；draining 保留，运维下线状态不能被重启抹掉
redis.call('HSET', KEYS[1], 'conns', 0, 'sessions', 0,
           'host', ARGV[1], 'rpc_host', ARGV[2], 'client_port', ARGV[3],
           'rpc_port', ARGV[4], 'capacity', ARGV[5], 'started_at', ARGV[6])
redis.call('PEXPIRE', KEYS[1], ARGV[7])
redis.call('SADD', KEYS[2], ARGV[8])
return 1
)lua";

//心跳：注册过期后返回 0，ChatServer 据此重新注册，而不是带着失效状态继续跑
const char* kHeartbeatScript = R"lua(
if redis.call('EXISTS', KEYS[1]) == 0 then return {0, 0} end
redis.call('HSET', KEYS[1], 'conns', ARGV[1], 'sessions', ARGV[2])
redis.call('PEXPIRE', KEYS[1], ARGV[3])
local draining = redis.call('HGET', KEYS[1], 'draining') or '0'
return {1, tonumber(draining)}
)lua";

const char* kDrainScript = R"lua(
if redis.call('EXISTS', KEYS[1]) == 0 then return 0 end
redis.call('HSET', KEYS[1], 'draining', 1)
redis.call('PEXPIRE', KEYS[1], ARGV[1])
return 1
)lua";

std::string InstanceKey(const std::string& name)
{
	return std::string(INSTANCEPREFIX) + name;
}

int ParseInt(const std::map<std::string, std::string>& fields, const std::string& key, int fallback)
{
	auto iter = fields.find(key);
	if (iter == fields.end() || iter->second.empty())
		return fallback;
	char* end = nullptr;
	const long value = std::strtol(iter->second.c_str(), &end, 10);
	if (end == iter->second.c_str())
		return fallback;
	return static_cast<int>(value);
}

} // namespace

InstanceRegistry::InstanceRegistry()
{
	//配置里的实例名是唯一允许注册的白名单，防止任意进程把自己登记成聊天实例
	auto& cfg = ConfigMgr::Inst();
	std::stringstream stream(cfg["chatservers"]["Name"]);
	std::string word;
	while (std::getline(stream, word, ',')) {
		if (!word.empty())
			_allowed.push_back(word);
	}
	if (_allowed.empty())
		std::cout << "[instance] 白名单为空，将接受任意实例注册，请在 [chatservers] Name 中配置" << std::endl;
}

InstanceRegistry& InstanceRegistry::Inst()
{
	static InstanceRegistry registry;
	return registry;
}

bool InstanceRegistry::Allowed(const std::string& name) const
{
	if (_allowed.empty())
		return true;
	return std::find(_allowed.begin(), _allowed.end(), name) != _allowed.end();
}

bool InstanceRegistry::Register(const std::string& name, const std::string& host,
	const std::string& rpc_host, int client_port, int rpc_port, int capacity,
	int& heartbeat_seconds, bool& draining)
{
	heartbeat_seconds = INSTANCE_HEARTBEAT_SECONDS;
	draining = false;
	if (name.empty() || host.empty() || client_port <= 0 || rpc_port <= 0)
		return false;
	//容器部署时客户端地址与控制面地址不同，未单独配置就退回客户端地址
	const std::string control_host = rpc_host.empty() ? host : rpc_host;

	if (!Allowed(name)) {
		std::cout << "[instance] register rejected name=" << name << " not in whitelist" << std::endl;
		return false;
	}

	//容量缺省或非法时按 1000 处理，避免除零与"容量 0 永远排在最后"
	if (capacity <= 0)
		capacity = 1000;

	const std::vector<std::string> keys{ InstanceKey(name), INSTANCE_SET };
	const std::vector<std::string> args{
		host, control_host, std::to_string(client_port), std::to_string(rpc_port),
		std::to_string(capacity),
		std::to_string(std::time(nullptr)),
		std::to_string(INSTANCE_TTL_SECONDS * 1000), name
	};

	auto* reply = RedisMgr::GetInstance()->Eval(kRegisterScript, keys, args);
	const bool ok = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
	freeReplyObject(reply);
	if (ok) {
		InstanceInfo info;
		if (Get(name, info))
			draining = info.draining;
		std::cout << "[instance] registered name=" << name << " " << host << ":"
			<< client_port << " rpc=" << rpc_port << std::endl;
	}
	return ok;
}

bool InstanceRegistry::Unregister(const std::string& name)
{
	if (name.empty())
		return false;
	//立即从集合与哈希中删除，新登录不再分配到正在退出的实例
	RedisMgr::GetInstance()->SRem(INSTANCE_SET, name);
	const bool removed = RedisMgr::GetInstance()->Del(InstanceKey(name));
	std::cout << "[instance] unregistered name=" << name << std::endl;
	return removed;
}

bool InstanceRegistry::Heartbeat(const std::string& name, int conns, int sessions, bool& draining)
{
	draining = false;
	if (name.empty())
		return false;

	const std::vector<std::string> keys{ InstanceKey(name) };
	const std::vector<std::string> args{
		std::to_string(std::max(conns, 0)), std::to_string(std::max(sessions, 0)),
		std::to_string(INSTANCE_TTL_SECONDS * 1000)
	};

	auto* reply = RedisMgr::GetInstance()->Eval(kHeartbeatScript, keys, args);
	if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
		freeReplyObject(reply);
		return false;
	}
	const bool exists = reply->element[0]->integer == 1;
	draining = reply->element[1]->integer == 1;
	freeReplyObject(reply);
	return exists;
}

bool InstanceRegistry::Drain(const std::string& name, int grace_seconds)
{
	if (name.empty())
		return false;
	//宽限期至少保留一个注册周期，避免实例还没收到通知就被清掉
	const int ttl = std::max(grace_seconds, INSTANCE_TTL_SECONDS);
	auto* reply = RedisMgr::GetInstance()->Eval(kDrainScript, { InstanceKey(name) },
		{ std::to_string(ttl * 1000) });
	const bool ok = reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
	freeReplyObject(reply);
	std::cout << "[instance] drain name=" << name << " grace=" << ttl << "s" << std::endl;
	return ok;
}

bool InstanceRegistry::Get(const std::string& name, InstanceInfo& info)
{
	std::map<std::string, std::string> fields;
	if (!RedisMgr::GetInstance()->HGetAll(InstanceKey(name), fields) || fields.empty())
		return false;

	auto host = fields.find("host");
	if (host == fields.end() || host->second.empty())
		return false;

	info.name = name;
	info.host = host->second;
	auto rpc_host = fields.find("rpc_host");
	info.rpc_host = (rpc_host != fields.end() && !rpc_host->second.empty())
		? rpc_host->second : info.host;
	info.client_port = ParseInt(fields, "client_port", 0);
	info.rpc_port = ParseInt(fields, "rpc_port", 0);
	info.capacity = ParseInt(fields, "capacity", 1000);
	info.conns = ParseInt(fields, "conns", 0);
	info.sessions = ParseInt(fields, "sessions", 0);
	info.draining = ParseInt(fields, "draining", 0) == 1;
	info.started_at = ParseInt(fields, "started_at", 0);
	return info.client_port > 0 && info.rpc_port > 0;
}

std::vector<InstanceInfo> InstanceRegistry::List()
{
	std::vector<InstanceInfo> result;
	std::vector<std::string> names;
	if (!RedisMgr::GetInstance()->SMembers(INSTANCE_SET, names))
		return result;

	//集合成员可能比哈希活得久，这里顺手清掉过期成员，避免集合无限增长
	std::vector<std::string> stale;
	for (const auto& name : names) {
		InstanceInfo info;
		if (Get(name, info))
			result.push_back(info);
		else
			stale.push_back(name);
	}
	for (const auto& name : stale)
		RedisMgr::GetInstance()->SRem(INSTANCE_SET, name);
	return result;
}

bool InstanceRegistry::Pick(InstanceInfo& info)
{
	const auto instances = List();
	bool found = false;
	for (const auto& candidate : instances) {
		if (candidate.draining || candidate.host.empty() ||
			candidate.client_port <= 0 || candidate.rpc_port <= 0)
			continue;
		if (!found) {
			info = candidate;
			found = true;
			continue;
		}
		//按 sessions/capacity 比较，用交叉相乘避免浮点误差
		const long long lhs = static_cast<long long>(candidate.sessions) * std::max(info.capacity, 1);
		const long long rhs = static_cast<long long>(info.sessions) * std::max(candidate.capacity, 1);
		if (lhs < rhs || (lhs == rhs && candidate.name < info.name)) {
			info = candidate;
		}
	}
	return found;
}
