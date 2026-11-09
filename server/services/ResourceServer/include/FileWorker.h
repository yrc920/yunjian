#pragma once
// 落盘线程：按文件名分派，同一文件的多个分片串行写入
//

#include <thread>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>
#include "FileTask.h"

class FileWorker
{
public:
	FileWorker();
	~FileWorker();

	void PostTask(std::shared_ptr<FileTask> task);
	void Stop();

private:
	void TaskCallback(std::shared_ptr<FileTask> task);
	void UpdateAvatarInfo(int uid, const std::string& name);

	std::thread _work_thread;
	std::queue<std::shared_ptr<FileTask>> _task_que;
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
};
