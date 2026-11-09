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

	void PostTask(std::shared_ptr<DownloadTask> task); //入队一个下载请求
	void Stop(); //停止并join

private:
	void TaskCallback(std::shared_ptr<DownloadTask> task); //按seq读一片并回调

	std::thread _work_thread;
	std::queue<std::shared_ptr<DownloadTask>> _task_que;
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
};
