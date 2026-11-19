#include "MysqlGuard.h"
#include "Log.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr auto kProbeInterval = std::chrono::seconds(2);
constexpr auto kProbeTimeoutMs = 3000;

long long NowMs() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

//可中断的休眠：Stop 后最迟 200ms 退出，不用等满整个周期
void SleepUnlessStopped(const std::atomic<bool>& stop, std::chrono::milliseconds total) {
	const auto deadline = std::chrono::steady_clock::now() + total;
	while (!stop.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

} // namespace

MysqlGuard& MysqlGuard::Instance() {
	//故意不析构：探针线程可能在进程退出的瞬间还在跑，对象活到进程结束最简单
	static MysqlGuard* guard = new MysqlGuard();
	return *guard;
}

void MysqlGuard::Start(std::function<void()> probe, const std::string& service_name) {
	std::lock_guard<std::mutex> lock(_mutex);
	if (_started)
		return;
	_probe = std::move(probe);
	_service = service_name;
	_started = true;
	_probe_thread = std::thread([this]() { ProbeLoop(); });
	_watchdog_thread = std::thread([this]() { WatchdogLoop(); });
}

void MysqlGuard::Stop() {
	if (!_started)
		return;
	_stop.store(true);
	//不 join：探针可能正卡在已暂停的数据库里，join 会把停服流程也挂住。
	//线程持有的是不析构的单例，进程退出时由系统回收。
	if (_probe_thread.joinable())
		_probe_thread.detach();
	if (_watchdog_thread.joinable())
		_watchdog_thread.detach();
}

void MysqlGuard::ProbeLoop() {
	while (!_stop.load()) {
		_probe_started_ms.store(NowMs());
		try {
			_probe();
			_probe_started_ms.store(0);
			if (_open.exchange(false))
				LOG_WARN << "[" << _service << "] mysql probe recovered, db breaker closed";
		}
		catch (const std::exception& e) {
			_probe_started_ms.store(0);
			if (!_open.exchange(true))
				LOG_ERROR << "[" << _service << "] mysql probe failed, db breaker opened: " << e.what();
		}
		SleepUnlessStopped(_stop, std::chrono::duration_cast<std::chrono::milliseconds>(kProbeInterval));
	}
}

void MysqlGuard::WatchdogLoop() {
	while (!_stop.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(200));
		const long long started = _probe_started_ms.load();
		if (started == 0 || _open.load())
			continue;
		if (NowMs() - started < kProbeTimeoutMs)
			continue;
		//探针发出去了却迟迟不回来：数据库没有响应，先挡住新请求
		_open.store(true);
		LOG_ERROR << "[" << _service << "] mysql probe stalled over " << kProbeTimeoutMs
			<< "ms, db breaker opened";
	}
}

bool MysqlGuard::AllowQuery() {
	if (!_started || !_open.load())
		return true;

	//熔断期间每个请求都会来这里，日志按 5 秒限速，避免刷屏
	const long long now = NowMs();
	long long last = _last_reject_log_ms.load();
	if (now - last > 5000 && _last_reject_log_ms.compare_exchange_strong(last, now))
		LOG_WARN << "[" << _service << "] db breaker open, rejecting query without waiting";

	return false;
}
