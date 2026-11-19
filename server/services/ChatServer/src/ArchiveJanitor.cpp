#include "ArchiveJanitor.h"

#include "ConfigMgr.h"
#include "MysqlDao.h"
#include "const.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include "Log.h"

namespace {

int ReadPositive(const char* key, int fallback)
{
	const std::string value = ConfigMgr::Inst().GetValue("Archive", key);
	if (value.empty())
		return fallback;
	const int parsed = std::atoi(value.c_str());
	return parsed >= 0 ? parsed : fallback; //0 在这里是合法值（比如保留当天），只挡负数
}

bool Enabled()
{
	const std::string value = ConfigMgr::Inst().GetValue("Archive", "Enabled");
	if (value.empty())
		return true;
	return value != "0"; //只有显式写 0 才关闭，配置写错时保持开启
}
}

ArchiveJanitor::~ArchiveJanitor()
{
	Stop();
}

void ArchiveJanitor::Start()
{
	if (_thread.joinable())
		return;
	_thread = std::thread(&ArchiveJanitor::Loop, this);
}

void ArchiveJanitor::Stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop.exchange(true))
			return;
	}
	_cv.notify_all();
	if (_thread.joinable())
		_thread.join();
}

void ArchiveJanitor::Loop()
{
	if (!Enabled())
		return;

	const int retention_days = ReadPositive("RetentionDays", DEFAULT_ARCHIVE_RETENTION_DAYS);
	const int interval_hours = ReadPositive("IntervalHours", DEFAULT_ARCHIVE_INTERVAL_HOURS);
	const int batch = ReadPositive("BatchSize", DEFAULT_ARCHIVE_BATCH);
	if (batch <= 0 || interval_hours <= 0) {
		LOG_ERROR << "[archive] invalid batch/interval, janitor disabled";
		return;
	}

	//启动后先等一会儿再跑第一次：避开启动时的高频数据库操作；
	//批量重启（发版、扩容）时也不会所有实例同时开始搬
	const int first_delay = ReadPositive("FirstDelaySeconds", DEFAULT_ARCHIVE_FIRST_DELAY_SECONDS);
	auto interval = std::chrono::seconds(first_delay > 0 ? first_delay : 1);

	std::unique_lock<std::mutex> lock(_mutex);
	while (!_stop.load()) {
		_cv.wait_for(lock, interval, [this] { return _stop.load(); });
		if (_stop.load())
			break;

		lock.unlock(); //搬运可能持续很久，不能占着锁阻塞 Stop
		try {
			const long long archived = MysqlDao::GetInstance()->ArchiveChatMessages(
				retention_days, batch);
			if (archived > 0) {
				LOG_INFO << "[archive] moved " << archived << " messages older than "
					<< retention_days << " days";
			}
			else if (archived < 0) {
				LOG_ERROR << "[archive] archive run failed, will retry next interval";
			}
		}
		catch (std::exception& e) {
			LOG_ERROR << "[archive] run failed: " << e.what();
		}
		lock.lock();

		interval = std::chrono::hours(interval_hours);
	}
}
