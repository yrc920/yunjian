#include "SessionRouter.h"
#include "StatusGrpcClient.h"
#include "const.h"
#include <algorithm>
#include <iostream>

namespace {

std::chrono::steady_clock::time_point ExpiryFor(bool online)
{
	const int seconds = online ? ROUTE_CACHE_SECONDS : ROUTE_NEGATIVE_CACHE_SECONDS;
	return std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

} // namespace

SessionRouter& SessionRouter::Inst()
{
	static SessionRouter router;
	return router;
}

void SessionRouter::Store(int uid, bool online, const std::string& instance)
{
	if (uid <= 0)
		return;

	std::lock_guard<std::mutex> guard(_mutex);
	CacheEntry entry;
	entry.online = online;
	entry.instance = instance;
	entry.expires = ExpiryFor(online);
	_cache[uid] = entry;
	TrimIfNeeded();
}

void SessionRouter::TrimIfNeeded()
{
	//只在超过上限时清理：先删过期项，仍然过多就丢弃最早写入的一批
	if (_cache.size() <= static_cast<size_t>(ROUTE_CACHE_MAX))
		return;

	const auto now = std::chrono::steady_clock::now();
	for (auto iter = _cache.begin(); iter != _cache.end();) {
		if (iter->second.expires <= now)
			iter = _cache.erase(iter);
		else
			++iter;
	}
	while (_cache.size() > static_cast<size_t>(ROUTE_CACHE_MAX))
		_cache.erase(_cache.begin());
}

void SessionRouter::Invalidate(int uid)
{
	std::lock_guard<std::mutex> guard(_mutex);
	_cache.erase(uid);
}

bool SessionRouter::Lookup(int uid, std::string& instance)
{
	instance.clear();
	if (uid <= 0)
		return false;

	{
		std::lock_guard<std::mutex> guard(_mutex);
		auto iter = _cache.find(uid);
		if (iter != _cache.end() && iter->second.expires > std::chrono::steady_clock::now()) {
			if (!iter->second.online)
				return false;
			instance = iter->second.instance;
			return !instance.empty();
		}
	}

	auto reply = StatusGrpcClient::GetInstance()->LookupRoute(uid);
	if (reply.error() != ErrorCodes::Success) {
		//查询失败时不写缓存，下一次必须重新问，不能把"查不到"当成"离线"
		return false;
	}

	if (reply.online() && !reply.instance().empty()) {
		Store(uid, true, reply.instance());
		instance = reply.instance();
		return true;
	}

	Store(uid, false, "");
	return false;
}

std::map<int, std::string> SessionRouter::LookupBatch(const std::vector<int>& uids)
{
	std::map<int, std::string> result;
	if (uids.empty())
		return result;

	std::vector<int> misses;
	misses.reserve(uids.size());
	{
		std::lock_guard<std::mutex> guard(_mutex);
		const auto now = std::chrono::steady_clock::now();
		for (int uid : uids) {
			if (uid <= 0)
				continue;
			auto iter = _cache.find(uid);
			if (iter == _cache.end() || iter->second.expires <= now) {
				misses.push_back(uid);
				continue;
			}
			if (iter->second.online)
				result[uid] = iter->second.instance;
		}
	}

	//未命中的按批回源，一次最多 200 个，避免单个请求体过大
	for (size_t offset = 0; offset < misses.size(); offset += 200) {
		const size_t end = std::min(misses.size(), offset + 200);
		std::vector<int> chunk(misses.begin() + offset, misses.begin() + end);
		auto reply = StatusGrpcClient::GetInstance()->BatchLookupRoute(chunk);
		if (reply.error() != ErrorCodes::Success) {
			std::cout << "[route] batch lookup failed size=" << chunk.size() << std::endl;
			continue;
		}

		for (const auto& entry : reply.entries()) {
			const bool online = entry.online() && !entry.instance().empty();
			Store(entry.uid(), online, entry.instance());
			if (online)
				result[entry.uid()] = entry.instance();
		}
	}
	return result;
}
