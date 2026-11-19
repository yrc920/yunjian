#pragma once
// 日志：五个服务共用一套初始化
//
// 两路输出——控制台（彩色）与轮转文件 logs/<服务名>.log；
// 采用异步 logger：业务线程只把日志塞进队列，磁盘 I/O 由后台线程完成，
// 消息路径与 IO 线程不会被写日志拖慢。进程退出前调用 Shutdown 刷盘。
//
// 用法：
//   Log::Init("ChatServer");          // main 开头，读 [Log] 配置
//   spdlog::info("uid={} ...", uid);  // 需要结构化字段时直接写
//   LOG_INFO << "text " << value;     // 沿用流式写法，兼容老代码
//   Log::Shutdown();                  // main 退出前

#include <sstream>
#include <string>

#include <spdlog/spdlog.h>

namespace Log {

//读取 config.ini 的 [Log] 段并初始化；配置缺失时退回控制台 + info 级别
void Init(const std::string& service);
//刷盘并回收后台线程；重复调用安全
void Shutdown();

//作用域守卫：放在 main 里，任何 return 路径都会刷盘
struct Guard {
	~Guard() { Shutdown(); }
};

//流式日志行：析构时按级别输出一条，兼容原有的 std::cout << a << b 写法
class Line
{
public:
	explicit Line(spdlog::level::level_enum level) : _level(level) {}
	~Line();

	Line(const Line&) = delete;
	Line& operator=(const Line&) = delete;

	template <typename T>
	Line& operator<<(const T& value)
	{
		_stream << value;
		return *this;
	}

private:
	spdlog::level::level_enum _level;
	std::ostringstream _stream;
};
}

#define LOG_TRACE Log::Line(spdlog::level::trace)
#define LOG_DEBUG Log::Line(spdlog::level::debug)
#define LOG_INFO Log::Line(spdlog::level::info)
#define LOG_WARN Log::Line(spdlog::level::warn)
#define LOG_ERROR Log::Line(spdlog::level::err)
