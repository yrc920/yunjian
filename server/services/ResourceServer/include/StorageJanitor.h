#pragma once
// 存储清理：定期删除上传中断留下的 .part 文件
//
// 只处理带 .part 后缀的文件，已完成的资源永远不会被它碰到；
// 首次启动 30 秒后先扫一遍，之后按 [Storage] SweepMinutes 巡检。
//

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class StorageJanitor
{
public:
	StorageJanitor() = default;
	~StorageJanitor();

	StorageJanitor(const StorageJanitor&) = delete;
	StorageJanitor& operator=(const StorageJanitor&) = delete;

	void Start();
	void Stop(); //幂等：重复调用只做一次

private:
	void Loop();

	std::atomic<bool> _stop{false};
	std::mutex _mutex;
	std::condition_variable _cv;
	std::thread _thread;
};
