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

	void PostTask(std::shared_ptr<FileTask> task); //入队一个分片
	void Stop(); //停止并join

private:
	void TaskCallback(std::shared_ptr<FileTask> task); //解码、写盘、推进进度
	void UpdateAvatarInfo(int uid, const std::string& name); //最后一片写user.icon与缓存

	std::thread _work_thread;
	std::queue<std::shared_ptr<FileTask>> _task_que;
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
};
