#pragma once
// 口令哈希线程池
//
// Argon2id 单次占 64 MiB 内存，Release 下约 40 ms、Debug 下约 1 s。
// GateServer 的 io_context 只有一个线程，直接在 IO 线程里做哈希会把
// 所有 HTTP 请求（包括验证码、心跳）一起卡住，因此哈希必须挪到独立线程。
//
// 并发数由 [Password] Threads 决定（默认 2）：每个线程峰值 64 MiB，
// 线程数就是同时进行的哈希数，也是内存上界的除数。

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

class PasswordPool
{
public:
	static PasswordPool& Instance();

	//投递一个哈希任务；队列满返回 false，调用方应立刻回可重试错误
	bool Post(std::function<void()> task);
	void Stop(); //处理完队列中剩余任务后回收线程

private:
	PasswordPool();
	~PasswordPool();
	PasswordPool(const PasswordPool&) = delete;
	PasswordPool& operator=(const PasswordPool&) = delete;

	void Loop();

	std::size_t _queue_limit;
	std::deque<std::function<void()>> _tasks;
	std::mutex _mutex;
	std::condition_variable _cv;
	std::vector<std::thread> _threads;
	bool _stop{false};
};
