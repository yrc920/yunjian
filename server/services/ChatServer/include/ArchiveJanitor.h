#pragma once
// 消息归档：把超过保留期的聊天记录搬到 chat_message_archive
//
// 只在 ChatServer 启动后跑：默认 24 小时一次，分批搬运，避免长事务与
// 一次性大删除把表锁住。两个实例同时运行也安全——按 message_id 分批 +
// INSERT IGNORE，谁先搬谁生效，不会重复。
//

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class ArchiveJanitor
{
public:
	ArchiveJanitor() = default;
	~ArchiveJanitor();

	ArchiveJanitor(const ArchiveJanitor&) = delete;
	ArchiveJanitor& operator=(const ArchiveJanitor&) = delete;

	void Start();
	void Stop(); //幂等

private:
	void Loop();

	std::atomic<bool> _stop{false};
	std::mutex _mutex;
	std::condition_variable _cv;
	std::thread _thread;
};
