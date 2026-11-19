#include "Log.h"

#include "ConfigMgr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace {

constexpr std::size_t kQueueSize = 8192; //异步队列：满了丢最旧的，不阻塞业务线程
constexpr int kDefaultFileMaxMB = 32;
constexpr int kDefaultFileCount = 5;

//日志系统是否可用：进程退出阶段静态对象析构时可能还会打日志，
//那时 registry 已经清空，再写会抛异常，这里直接丢弃
std::atomic<bool> g_ready{false};

spdlog::level::level_enum ParseLevel(const std::string& text)
{
	std::string lowered = text;
	std::transform(lowered.begin(), lowered.end(), lowered.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

	if (lowered == "trace") return spdlog::level::trace;
	if (lowered == "debug") return spdlog::level::debug;
	if (lowered == "info" || lowered.empty()) return spdlog::level::info;
	if (lowered == "warn" || lowered == "warning") return spdlog::level::warn;
	if (lowered == "error" || lowered == "err") return spdlog::level::err;
	if (lowered == "critical") return spdlog::level::critical;
	if (lowered == "off") return spdlog::level::off;
	return spdlog::level::info; //写错级别按 info 处理，不因为配置笔误把日志关掉
}

int ReadPositive(const std::string& text, int fallback)
{
	if (text.empty())
		return fallback;
	const int parsed = std::atoi(text.c_str());
	return parsed > 0 ? parsed : fallback;
}
}

void Log::Init(const std::string& service)
{
	static bool initialized = false;
	if (initialized)
		return;

	//配置缺失（比如启动目录不对）不能让服务起不来：退回控制台 + info
	std::string level_text = "info";
	std::string dir = "logs";
	int file_max_mb = kDefaultFileMaxMB;
	int file_count = kDefaultFileCount;
	bool console = true;
	try {
		auto& cfg = ConfigMgr::Inst();
		level_text = cfg.GetValue("Log", "Level");
		const std::string dir_cfg = cfg.GetValue("Log", "Dir");
		if (!dir_cfg.empty())
			dir = dir_cfg;
		file_max_mb = ReadPositive(cfg.GetValue("Log", "FileMaxMB"), kDefaultFileMaxMB);
		file_count = ReadPositive(cfg.GetValue("Log", "FileCount"), kDefaultFileCount);
		console = cfg.GetValue("Log", "Console") != "0";
	}
	catch (std::exception& e) {
		std::cerr << "log config unavailable, fallback to console: " << e.what() << std::endl;
	}

	std::vector<spdlog::sink_ptr> sinks;
	if (console) {
		auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
		sinks.push_back(console_sink);
	}

	try {
		std::filesystem::create_directories(dir);
		const std::string file = dir + "/" + service + ".log";
		auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
			file, static_cast<std::size_t>(file_max_mb) * 1024 * 1024,
			static_cast<std::size_t>(file_count));
		sinks.push_back(file_sink);
	}
	catch (std::exception& e) {
		//目录不可写时只留控制台，服务照常运行
		std::cerr << "log file sink disabled: " << e.what() << std::endl;
	}

	if (sinks.empty())
		sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

	spdlog::init_thread_pool(kQueueSize, 1);
	auto logger = std::make_shared<spdlog::async_logger>(
		service, sinks.begin(), sinks.end(), spdlog::thread_pool(),
		spdlog::async_overflow_policy::overrun_oldest);
	logger->set_level(ParseLevel(level_text));
	logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
	logger->flush_on(spdlog::level::warn); //警告及以上立刻落盘，崩溃前的现场不会丢

	spdlog::set_default_logger(logger);
	spdlog::flush_every(std::chrono::seconds(2));
	initialized = true;
	g_ready.store(true);

	//把生效的日志配置写一行，排障时先确认"看到的是不是这套设置"
	logger->info("log ready service={} level={} dir={} rotate={}MB x{} console={}",
		service, level_text, dir, file_max_mb, file_count, console);
}

void Log::Shutdown()
{
	g_ready.store(false);
	spdlog::shutdown();
}

Log::Line::~Line()
{
	std::string text = _stream.str();
	//调用方沿用 std::endl 收尾，去掉末尾换行，避免每条日志后面多一个空行
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
		text.pop_back();
	if (text.empty() || !g_ready.load())
		return;

	try {
		auto logger = spdlog::default_logger();
		if (logger)
			logger->log(_level, "{}", text);
	}
	catch (...) {
		//日志系统已关闭，丢弃这一条，不能让它影响正常的退出流程
	}
}
