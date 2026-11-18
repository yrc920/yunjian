#include "IOContextPool.h"

//hardware_concurrency 可能返回 0，至少保留一个事件循环，避免轮询取用时访问空数组
IOContextPool::IOContextPool(std::size_t size)
	: _ioContexts(size == 0 ? 1 : size), _works(size == 0 ? 1 : size), _nextIOContext(0)
{
	for (std::size_t i = 0; i < _ioContexts.size(); ++i)
		_works[i] = std::make_unique<Work>(boost::asio::make_work_guard(_ioContexts[i].get_executor()));

	for (std::size_t i = 0; i < _ioContexts.size(); ++i) {
		_threads.emplace_back([this, i]() { _ioContexts[i].run(); });
	}
}

IOContextPool::~IOContextPool()
{
	Stop(); //重复调用安全：空 work 的 reset 与已 join 的线程都不再处理
}

boost::asio::io_context& IOContextPool::GetIOContext()
{
	auto& context = _ioContexts[_nextIOContext++];
	if (_nextIOContext == _ioContexts.size())
		_nextIOContext = 0; //回到第一个，继续轮询
	return context;
}

void IOContextPool::Stop()
{
	//由拥有线程池的线程调用，不能在被等待的工作线程自身调用
	for (auto& context : _ioContexts) {
		context.stop(); //让 run 尽快返回，不等未完成的网络操作
	}
	for (auto& work : _works) {
		work.reset();
	}
	for (auto& thread : _threads) {
		if (thread.joinable())
			thread.join();
	}
}

void IOContextPool::StopAfterDrain()
{
	for (auto& work : _works) {
		work.reset(); //不再保活，已有回调处理完后 run 自然返回
	}
	for (auto& thread : _threads) {
		if (thread.joinable())
			thread.join();
	}
}
