#include "IOContextPool.h"
#include <iostream>

IOContextPool::IOContextPool(std::size_t size) : _ioContexts(size == 0 ? 1 : size),
_works(size == 0 ? 1 : size), _nextIOContext(0)
{
	for (std::size_t i = 0; i < _ioContexts.size(); ++i) {
		_works[i] = std::make_unique<Work>(boost::asio::make_work_guard(_ioContexts[i].get_executor())); //防止run空转退出
	}

	for (std::size_t i = 0; i < _ioContexts.size(); ++i) {
		_threads.emplace_back([this, i]() {
			_ioContexts[i].run(); //每个线程跑一个事件循环
		});
	}
}

IOContextPool::~IOContextPool() {
	Stop();
}

boost::asio::io_context& IOContextPool::GetIOContext() {
	auto& service = _ioContexts[_nextIOContext++]; //轮询取
	if (_nextIOContext == _ioContexts.size()) {
		_nextIOContext = 0; //回到第一个
	}
	return service;
}

void IOContextPool::Stop() {
	//由拥有线程池的线程调用，不能在池内线程里等待自身退出
	for (auto& context : _ioContexts) {
		context.stop(); //丢弃未执行回调
	}
	for (auto& work : _works) {
		work.reset(); //释放保活
	}
	for (auto& thread : _threads) {
		if (thread.joinable()) {
			thread.join(); //等待线程退出
		}
	}
}

void IOContextPool::StopAfterDrain() {
	for (auto& work : _works) {
		work.reset(); //不再保活，已有回调处理完run自然返回
	}
	for (auto& thread : _threads) {
		if (thread.joinable())
			thread.join(); //正常停服不丢请求
	}
}
