#include "SessionRegistry.h"
#include "RedisMgr.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include "const.h"
#include "Log.h"

namespace {

//申请租约：同一 session_id 重入直接返回原 epoch，避免客户端重试把 epoch 推高
//epoch 取 max(计数器+1, Redis 当前毫秒)，Redis 被清空后也不会回退到旧值以下
const char* kAllocateScript = R"lua(
local cur_sid = redis.call('HGET', KEYS[1], 'sid')
if cur_sid == ARGV[1] then
  redis.call('PEXPIRE', KEYS[1], ARGV[3])
  local e = redis.call('HGET', KEYS[1], 'epoch') or '0'
  return {0, tonumber(e), '', ''}
end
local cnt = redis.call('INCR', KEYS[3])
if cnt == 1 then redis.call('PEXPIRE', KEYS[3], ARGV[4]) end
if cnt > tonumber(ARGV[5]) then return {-1, 0, '', ''} end
local t = redis.call('TIME')
local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local epoch = tonumber(redis.call('GET', KEYS[2]) or '0') + 1
if epoch < now then epoch = now end
local prev_inst = redis.call('HGET', KEYS[1], 'inst') or ''
local prev_sid = redis.call('HGET', KEYS[1], 'sid') or ''
redis.call('SET', KEYS[2], epoch)
redis.call('HSET', KEYS[1], 'sid', ARGV[1], 'inst', ARGV[2], 'epoch', epoch)
redis.call('PEXPIRE', KEYS[1], ARGV[3])
return {1, epoch, prev_inst, prev_sid}
)lua";

//续租：租约不存在说明控制面中断期间过期了，此时按原 session_id 重新授予，
//不能让一次状态服务故障把在线用户全部踢下线
const char* kRenewScript = R"lua(
local cur_sid = redis.call('HGET', KEYS[1], 'sid')
if cur_sid == ARGV[1] then
  redis.call('PEXPIRE', KEYS[1], ARGV[2])
  local e = redis.call('HGET', KEYS[1], 'epoch') or '0'
  return {0, tonumber(e)}
end
if cur_sid then return {2, 0} end
local t = redis.call('TIME')
local now = tonumber(t[1]) * 1000 + math.floor(tonumber(t[2]) / 1000)
local epoch = tonumber(redis.call('GET', KEYS[2]) or '0') + 1
if epoch < now then epoch = now end
redis.call('SET', KEYS[2], epoch)
redis.call('HSET', KEYS[1], 'sid', ARGV[1], 'inst', ARGV[3], 'epoch', epoch)
redis.call('PEXPIRE', KEYS[1], ARGV[2])
return {1, epoch}
)lua";

//释放：session_id 与 epoch 必须同时匹配，旧连接的清理永远删不掉新会话
const char* kReleaseScript = R"lua(
local cur_sid = redis.call('HGET', KEYS[1], 'sid')
if not cur_sid then return 0 end
local cur_epoch = tonumber(redis.call('HGET', KEYS[1], 'epoch') or '0')
if cur_sid ~= ARGV[1] or cur_epoch ~= tonumber(ARGV[2]) then return 0 end
redis.call('DEL', KEYS[1])
return 1
)lua";

//登出：会话匹配时同时删除租约与令牌；被顶掉的旧连接不能用它吊销新令牌
const char* kLogoutScript = R"lua(
local cur_sid = redis.call('HGET', KEYS[1], 'sid')
if not cur_sid then return 0 end
local cur_epoch = tonumber(redis.call('HGET', KEYS[1], 'epoch') or '0')
if cur_sid ~= ARGV[1] or cur_epoch ~= tonumber(ARGV[2]) then return 0 end
redis.call('DEL', KEYS[1])
redis.call('DEL', KEYS[2])
return 1
)lua";

//批量查询：一次往返取回所有 uid，返回 sid/inst/epoch 三元组顺序展平
const char* kBatchLookupScript = R"lua(
local out = {}
for i = 1, #KEYS do
  local v = redis.call('HMGET', KEYS[i], 'sid', 'inst', 'epoch')
  out[#out + 1] = v[1] or ''
  out[#out + 1] = v[2] or ''
  out[#out + 1] = v[3] or '0'
end
return out
)lua";

std::string SessionKey(int uid)
{
	return std::string(SESSIONPREFIX) + std::to_string(uid);
}

std::string EpochKey(int uid)
{
	return std::string(EPOCHPREFIX) + std::to_string(uid);
}

std::string RateKey(int uid)
{
	return std::string(SESSIONPREFIX) + "rate_" + std::to_string(uid);
}

long long ReplyInt(redisReply* reply, size_t index)
{
	if (!reply || reply->type != REDIS_REPLY_ARRAY || index >= reply->elements)
		return 0;
	const auto* element = reply->element[index];
	if (!element)
		return 0;
	if (element->type == REDIS_REPLY_INTEGER)
		return element->integer;
	if (element->type == REDIS_REPLY_STRING)
		return std::strtoll(element->str, nullptr, 10);
	return 0;
}

std::string ReplyString(redisReply* reply, size_t index)
{
	if (!reply || reply->type != REDIS_REPLY_ARRAY || index >= reply->elements)
		return "";
	const auto* element = reply->element[index];
	if (!element || element->type != REDIS_REPLY_STRING)
		return "";
	return std::string(element->str, element->len);
}

} // namespace

SessionRegistry::SessionRegistry() : _shards(64) {}

SessionRegistry& SessionRegistry::Inst()
{
	static SessionRegistry registry;
	return registry;
}

std::mutex& SessionRegistry::Shard(int uid)
{
	//uid 为负数时先转无符号，避免取模得到负下标
	return _shards[static_cast<size_t>(static_cast<unsigned int>(uid)) % _shards.size()];
}

bool SessionRegistry::Allocate(int uid, const std::string& session_id,
	const std::string& instance, long long& epoch, std::string& kicked_instance,
	std::string& kicked_session_id, bool& rate_limited)
{
	epoch = 0;
	kicked_instance.clear();
	kicked_session_id.clear();
	rate_limited = false;
	if (uid <= 0 || session_id.empty() || instance.empty())
		return false;

	//同一 uid 的租约变更在这里串行；跨进程由 Lua 的原子性保证
	std::lock_guard<std::mutex> guard(Shard(uid));
	const std::vector<std::string> keys{ SessionKey(uid), EpochKey(uid), RateKey(uid) };
	const std::vector<std::string> args{
		session_id, instance,
		std::to_string(LEASE_TTL_SECONDS * 1000),
		std::to_string(ALLOCATE_RATE_WINDOW_SECONDS * 1000),
		std::to_string(ALLOCATE_RATE_LIMIT)
	};

	auto* reply = RedisMgr::GetInstance()->Eval(kAllocateScript, keys, args);
	if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 4) {
		freeReplyObject(reply);
		LOG_ERROR << "[session] allocate failed uid=" << uid << " redis_error";
		return false;
	}

	const long long code = ReplyInt(reply, 0);
	epoch = ReplyInt(reply, 1);
	kicked_instance = ReplyString(reply, 2);
	kicked_session_id = ReplyString(reply, 3);
	freeReplyObject(reply);

	if (code < 0) {
		rate_limited = true;
		LOG_WARN << "[session] allocate rate limited uid=" << uid;
		return false;
	}
	return true;
}

int SessionRegistry::Renew(int uid, const std::string& session_id,
	const std::string& instance, long long& epoch)
{
	epoch = 0;
	if (uid <= 0 || session_id.empty() || instance.empty())
		return 3;

	std::lock_guard<std::mutex> guard(Shard(uid));
	const std::vector<std::string> keys{ SessionKey(uid), EpochKey(uid) };
	const std::vector<std::string> args{
		session_id, std::to_string(LEASE_TTL_SECONDS * 1000), instance
	};

	auto* reply = RedisMgr::GetInstance()->Eval(kRenewScript, keys, args);
	if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 2) {
		freeReplyObject(reply);
		return 3;
	}
	const int status = static_cast<int>(ReplyInt(reply, 0));
	epoch = ReplyInt(reply, 1);
	freeReplyObject(reply);
	return status;
}

bool SessionRegistry::Release(int uid, const std::string& session_id,
	long long epoch, bool& released)
{
	released = false;
	if (uid <= 0 || session_id.empty())
		return false;

	std::lock_guard<std::mutex> guard(Shard(uid));
	const std::vector<std::string> keys{ SessionKey(uid) };
	const std::vector<std::string> args{ session_id, std::to_string(epoch) };

	auto* reply = RedisMgr::GetInstance()->Eval(kReleaseScript, keys, args);
	if (!reply) {
		return false;
	}
	//脚本返回整数 0/1；未被删除同样属于正常结果，不算失败
	released = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
	freeReplyObject(reply);
	return true;
}

bool SessionRegistry::Logout(int uid, const std::string& session_id,
	long long epoch, bool& revoked)
{
	revoked = false;
	if (uid <= 0 || session_id.empty())
		return false;

	std::lock_guard<std::mutex> guard(Shard(uid));
	const std::vector<std::string> keys{
		SessionKey(uid),
		std::string(USERTOKENPREFIX) + std::to_string(uid)
	};
	const std::vector<std::string> args{ session_id, std::to_string(epoch) };

	auto* reply = RedisMgr::GetInstance()->Eval(kLogoutScript, keys, args);
	if (!reply)
		return false;

	//已被顶替或已过期时返回 0：租约与令牌都不动，新会话不受影响
	revoked = reply->type == REDIS_REPLY_INTEGER && reply->integer == 1;
	freeReplyObject(reply);
	return true;
}

bool SessionRegistry::Lookup(int uid, SessionLease& lease)
{
	if (uid <= 0)
		return false;

	std::map<std::string, std::string> fields;
	if (!RedisMgr::GetInstance()->HGetAll(SessionKey(uid), fields))
		return false;

	auto sid = fields.find("sid");
	auto inst = fields.find("inst");
	auto epoch = fields.find("epoch");
	//键过期或字段不完整都按离线处理，绝不返回半条租约
	if (sid == fields.end() || inst == fields.end() || epoch == fields.end())
		return false;
	if (sid->second.empty() || inst->second.empty())
		return false;

	lease.session_id = sid->second;
	lease.instance = inst->second;
	lease.epoch = std::strtoll(epoch->second.c_str(), nullptr, 10);
	return true;
}

std::map<int, SessionLease> SessionRegistry::BatchLookup(const std::vector<int>& uids)
{
	std::map<int, SessionLease> result;
	if (uids.empty())
		return result;

	//单次上限 200，超过的部分交给调用方分批，避免脚本过长阻塞 Redis
	const size_t count = std::min(uids.size(), static_cast<size_t>(MAX_BATCH_LOOKUP));
	std::vector<std::string> keys;
	std::vector<int> used_uids; //与 keys 一一对应，uid 可能被过滤，不能再用原下标反查
	keys.reserve(count);
	used_uids.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		if (uids[i] > 0) {
			keys.push_back(SessionKey(uids[i]));
			used_uids.push_back(uids[i]);
		}
	}
	if (keys.empty())
		return result;

	auto* reply = RedisMgr::GetInstance()->Eval(kBatchLookupScript, keys, {});
	if (!reply || reply->type != REDIS_REPLY_ARRAY) {
		freeReplyObject(reply);
		return result;
	}

	for (size_t i = 0; i + 2 < reply->elements; i += 3) {
		const size_t index = i / 3;
		if (index >= used_uids.size())
			break;
		SessionLease lease;
		lease.session_id = ReplyString(reply, i);
		lease.instance = ReplyString(reply, i + 1);
		lease.epoch = ReplyInt(reply, i + 2);
		if (lease.session_id.empty() || lease.instance.empty())
			continue;
		result[used_uids[index]] = lease;
	}
	freeReplyObject(reply);
	return result;
}
