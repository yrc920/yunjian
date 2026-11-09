#pragma once
// 下载线程：读取指定分片并做Base64编码
//

#include <thread>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "FileTask.h"

class DownloadWorker
{
public:
	DownloadWorker();
	~DownloadWorker();

	void PostTask(std::shared_ptr<DownloadTask> task);
	void Stop();

private:
	void TaskCallback(std::shared_ptr<DownloadTask> task);

	std::thread _work_thread;
	std::queue<std::shared_ptr<DownloadTask>> _task_que;
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
};
