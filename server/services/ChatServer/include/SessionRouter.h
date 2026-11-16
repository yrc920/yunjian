#pragma once
// 在线位置路由：本地缓存 + 未命中回源 StatusServer
// 缓存只是加速，权威始终在租约表；消息投递失败时会失效并重试一次
//

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class SessionRouter
{
public:
	static SessionRouter& Inst();

	// 查询单个用户所在实例，离线或查询失败返回 false
	bool Lookup(int uid, std::string& instance);

	// 批量查询，返回 uid -> 实例名；离线的 uid 不出现在结果里
	std::map<int, std::string> LookupBatch(const std::vector<int>& uids);

	// 投递失败或收到"目标不在本实例"时失效缓存，下一次重新回源
	void Invalidate(int uid);

private:
	SessionRouter() = default;

	struct CacheEntry {
		std::string instance;
		std::chrono::steady_clock::time_point expires;
		bool online = false;
	};

	void Store(int uid, bool online, const std::string& instance);
	void TrimIfNeeded();

	std::mutex _mutex;
	std::unordered_map<int, CacheEntry> _cache;
};
