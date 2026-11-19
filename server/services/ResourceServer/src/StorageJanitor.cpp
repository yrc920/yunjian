#include "StorageJanitor.h"

#include "ConfigMgr.h"
#include "const.h"

#include <boost/filesystem.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include "Log.h"

namespace {

int ReadPositive(const char* key, int fallback)
{
	const std::string value = ConfigMgr::Inst().GetValue("Storage", key);
	if (value.empty())
		return fallback;
	const int parsed = std::atoi(value.c_str());
	return parsed > 0 ? parsed : fallback; //非法配置退回默认值，不因配置写错停掉清理
}

//删除超过保留时长的 .part 文件，返回删除数量
int SweepStaleParts(int stale_hours)
{
	boost::system::error_code ec;
	const auto root = ConfigMgr::Inst().GetStoragePath();
	if (!boost::filesystem::exists(root, ec))
		return 0;

	const std::time_t now = std::time(nullptr);
	const auto stale_seconds = static_cast<long long>(stale_hours) * 3600;
	int removed = 0;

	boost::filesystem::recursive_directory_iterator it(root, ec), end;
	while (it != end && !ec) {
		const auto path = it->path();
		if (boost::filesystem::is_regular_file(path, ec)
			&& path.extension() == UPLOAD_PART_SUFFIX) {
			const std::time_t written = boost::filesystem::last_write_time(path, ec);
			if (!ec && static_cast<long long>(now - written) > stale_seconds) {
				boost::filesystem::remove(path, ec);
				if (!ec) {
					++removed;
					LOG_INFO << "[janitor] removed stale upload " << path.string();
				}
			}
		}
		it.increment(ec);
	}
	return removed;
}
}

StorageJanitor::~StorageJanitor()
{
	Stop();
}

void StorageJanitor::Start()
{
	if (_thread.joinable())
		return;
	_thread = std::thread(&StorageJanitor::Loop, this);
}

void StorageJanitor::Stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop.exchange(true))
			return; //已经停过
	}
	_cv.notify_all();
	if (_thread.joinable())
		_thread.join();
}

void StorageJanitor::Loop()
{
	const int stale_hours = ReadPositive("StaleUploadHours", DEFAULT_STALE_HOURS);
	const int sweep_minutes = ReadPositive("SweepMinutes", DEFAULT_SWEEP_MINUTES);

	//首次很快扫一遍：把上次进程崩溃留下的半成品清掉，之后按配置间隔巡检
	auto interval = std::chrono::seconds(30);

	std::unique_lock<std::mutex> lock(_mutex);
	while (!_stop.load()) {
		_cv.wait_for(lock, interval, [this] { return _stop.load(); });
		if (_stop.load())
			break;

		lock.unlock(); //扫描可能很慢，不能占着锁阻塞 Stop
		try {
			SweepStaleParts(stale_hours);
		}
		catch (std::exception& e) {
			LOG_ERROR << "[janitor] sweep failed: " << e.what();
		}
		lock.lock();

		interval = std::chrono::minutes(sweep_minutes);
	}
}
