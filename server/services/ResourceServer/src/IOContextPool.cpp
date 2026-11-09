#include "IOContextPool.h"
#include <iostream>

//hardware_concurrency可能返回0，至少保留一个事件循环，避免轮询取用时访问空数组
IOContextPool::IOContextPool(std::size_t size) : _ioContexts(size == 0 ? 1 : size),
_works(size == 0 ? 1 : size), _nextIOContext(0)
{
	for (std::size_t i = 0; i < _ioContexts.size(); ++i) {
		_works[i] = std::make_unique<Work>(boost::asio::make_work_guard(_ioContexts[i].get_executor()));
	}

	for (std::size_t i = 0; i < _ioContexts.size(); ++i) {
		_threads.emplace_back([this, i]() {
			_ioContexts[i].run();
		});
	}
}

IOContextPool::~IOContextPool() {
	Stop();
}

boost::asio::io_context& IOContextPool::GetIOContext() {
	auto& service = _ioContexts[_nextIOContext++];
	if (_nextIOContext == _ioContexts.size()) {
		_nextIOContext = 0;
	}
	return service;
}

void IOContextPool::Stop() {
	//由拥有线程池的线程调用，不能在池内线程里等待自身退出
	for (auto& context : _ioContexts) {
		context.stop();
	}
	for (auto& work : _works) {
		work.reset();
	}
	for (auto& thread : _threads) {
		if (thread.joinable()) {
			thread.join();
		}
	}
}

void IOContextPool::StopAfterDrain() {
	for (auto& work : _works) {
		work.reset();
	}
	for (auto& thread : _threads) {
		if (thread.joinable())
			thread.join();
	}
}
