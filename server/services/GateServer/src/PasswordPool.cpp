#include "PasswordPool.h"

#include "ConfigMgr.h"
#include "const.h"

#include <cstdlib>
#include <iostream>
#include "Log.h"

namespace {

std::size_t ReadCount(const char* key, int fallback, int max_value)
{
	const std::string value = ConfigMgr::Inst().GetValue("Password", key);
	if (value.empty())
		return static_cast<std::size_t>(fallback);
	const int parsed = std::atoi(value.c_str());
	if (parsed <= 0)
		return static_cast<std::size_t>(fallback);
	return static_cast<std::size_t>(parsed > max_value ? max_value : parsed);
}
}

PasswordPool& PasswordPool::Instance()
{
	static PasswordPool pool; //首次调用时才建线程，配置已就绪
	return pool;
}

PasswordPool::PasswordPool()
{
	//默认 2 个哈希线程：同时最多 128 MiB 的 Argon2 工作内存
	const std::size_t threads = ReadCount("Threads", DEFAULT_PASSWORD_THREADS, 8);
	_queue_limit = ReadCount("QueueLimit", DEFAULT_PASSWORD_QUEUE, 256);

	for (std::size_t i = 0; i < threads; ++i)
		_threads.emplace_back(&PasswordPool::Loop, this);
}

PasswordPool::~PasswordPool()
{
	Stop();
}

bool PasswordPool::Post(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop)
			return false;
		if (_tasks.size() >= _queue_limit)
			return false; //队列满：让调用方回可重试错误，不在这里阻塞 IO 线程
		_tasks.push_back(std::move(task));
	}
	_cv.notify_one();
	return true;
}

void PasswordPool::Stop()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop)
			return; //重复调用只停一次
		_stop = true;
	}
	_cv.notify_all();
	for (auto& thread : _threads) {
		if (thread.joinable())
			thread.join(); //线程会先把队列里剩余任务处理完
	}
	_threads.clear();
}

void PasswordPool::Loop()
{
	for (;;) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_cv.wait(lock, [this] { return _stop || !_tasks.empty(); });
			if (_tasks.empty())
				return; //已停服且没有待处理任务
			task = std::move(_tasks.front());
			_tasks.pop_front();
		}
		try {
			task();
		}
		catch (std::exception& e) {
			//单个任务失败不能带走工作线程，响应由任务自己回投
			LOG_ERROR << "[password] task failed: " << e.what();
		}
	}
}
