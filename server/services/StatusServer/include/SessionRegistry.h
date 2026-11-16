#pragma once
// 会话租约注册表：uid 维度的在线状态唯一权威
// 状态存放在 Redis，进程内只做按 uid 分片的串行化，重启不丢租约
//

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// 一条在线会话：谁（session_id）在哪台实例上，围栏令牌是多少
struct SessionLease {
	std::string session_id;
	std::string instance;
	long long epoch = 0;
};

class SessionRegistry
{
public:
	static SessionRegistry& Inst();

	// 申请租约：session_id 相同视为重复请求，直接返回原 epoch
	// 返回 false 表示 Redis 不可用；rate_limited 单独标记，便于调用方区分错误码
	bool Allocate(int uid, const std::string& session_id, const std::string& instance,
		long long& epoch, std::string& kicked_instance, std::string& kicked_session_id,
		bool& rate_limited);

	// 续租：租约不存在时重新授予（控制面短暂故障后不让用户掉线）
	// status 0 正常 / 1 重新授予 / 2 已被更高 epoch 顶替 / 3 内部错误
	int Renew(int uid, const std::string& session_id, const std::string& instance,
		long long& epoch);

	// 释放：只有 session_id 与 epoch 都匹配才删除，旧连接的清理不会误删新会话
	bool Release(int uid, const std::string& session_id, long long epoch, bool& released);

	// 显式登出：sid 与 epoch 都匹配时原子删除租约与令牌，返回是否删除
	bool Logout(int uid, const std::string& session_id, long long epoch, bool& revoked);

	// 查询单个 uid 的在线位置，未命中返回 false
	bool Lookup(int uid, SessionLease& lease);

	// 批量查询，未在线的 uid 不出现在结果里
	std::map<int, SessionLease> BatchLookup(const std::vector<int>& uids);

private:
	SessionRegistry();

	// 按 uid 取条带锁，保证同一用户的"读-判断-写"在单进程内串行
	std::mutex& Shard(int uid);

	std::vector<std::mutex> _shards;
};
