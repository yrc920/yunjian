#pragma once
// io_context 线程池，每个 io_context 独占一个工作线程
//

#include <vector>
#include <boost/asio.hpp>
#include "Singleton.h"

class IOContextPool : public Singleton<IOContextPool>
{
	friend Singleton<IOContextPool>;
public:
	using IOContext = boost::asio::io_context;
	using Work = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
	using WorkPtr = std::unique_ptr<Work>;

	~IOContextPool();
	IOContextPool(const IOContextPool&) = delete;
	IOContextPool& operator=(const IOContextPool&) = delete;

	boost::asio::io_context& GetIOContext(); //按轮询取一个 io_context
	void Stop(); //停止事件循环并回收线程
	void StopAfterDrain(); //释放保活，处理完已投递回调后回收线程
private:
	explicit IOContextPool(std::size_t size = std::thread::hardware_concurrency());

	std::vector<IOContext> _ioContexts;
	std::vector<WorkPtr> _works;
	std::vector<std::thread> _threads;
	std::size_t _nextIOContext;
};
