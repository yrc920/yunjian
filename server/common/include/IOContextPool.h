#pragma once
// io_context 线程池：每个 io_context 独占一个工作线程，按轮询取用
//

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>
#include <boost/asio.hpp>
#include "Singleton.h"

class IOContextPool : public Singleton<IOContextPool>
{
	friend Singleton<IOContextPool>;
public:
	using IOContext = boost::asio::io_context;
	//work guard 让 io_context 在没有任务时也保持运行
	using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	using WorkPtr = std::unique_ptr<Work>;

	IOContextPool(const IOContextPool&) = delete;
	IOContextPool& operator=(const IOContextPool&) = delete;
	~IOContextPool();

	boost::asio::io_context& GetIOContext(); //轮询取一个事件循环
	void Stop(); //停止事件循环，丢弃未执行的回调
	//释放保活对象，处理完已投递的回调后等待线程退出（正常停服用这个）
	void StopAfterDrain();

private:
	explicit IOContextPool(std::size_t size = std::thread::hardware_concurrency());

	std::vector<IOContext> _ioContexts;
	std::vector<WorkPtr> _works;
	std::vector<std::thread> _threads;
	std::size_t _nextIOContext;
};
