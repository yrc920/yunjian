#pragma once
// MySQL 健康熔断
//
// 单条查询的超时由连接自己管（每条都带 deadline，见 MySqlSession）。熔断解决的是
// 另一件事：数据库整体不响应时，不要让每个请求都去等满那一个 deadline。
//
//   · 探针线程每 2 秒用**独立会话**做一次 SELECT 1，不占业务连接池；
//   · 看门狗发现探针超过 3 秒没回来，就把熔断打开；
//   · 取连接前问一次 AllowQuery()，打开时立刻返回失败（实测 1–2ms）；
//   · 探针恢复成功后自动合闸，不用重启进程。

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class MysqlGuard {
public:
	static MysqlGuard& Instance();

	//probe 在探针线程里执行一次 SELECT 1：成功正常返回，失败抛异常
	void Start(std::function<void()> probe, const std::string& service_name);
	void Stop(); //停探针线程；卡在查询里时只置标志，线程自行退出

	//业务线程取连接前调用；false 表示熔断打开，应直接按"数据库暂不可用"返回
	bool AllowQuery();

private:
	MysqlGuard() = default;
	MysqlGuard(const MysqlGuard&) = delete;
	MysqlGuard& operator=(const MysqlGuard&) = delete;

	void ProbeLoop();
	void WatchdogLoop();

	std::function<void()> _probe;
	std::string _service;
	std::thread _probe_thread;
	std::thread _watchdog_thread;
	std::mutex _mutex;
	bool _started = false;
	std::atomic<bool> _stop{ false };
	std::atomic<bool> _open{ false };
	std::atomic<long long> _probe_started_ms{ 0 }; //0 表示当前没有探针在跑
	std::atomic<long long> _last_reject_log_ms{ 0 };
};
