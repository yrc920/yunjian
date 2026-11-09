#pragma once
// 单个逻辑线程：从自己的队列取消息并调用注册的处理器
//

#include <thread>
#include <queue>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <memory>
#include "CSession.h"

using ResourceCallback = std::function<void(std::shared_ptr<CSession>,
	std::uint16_t, const std::string&)>;

class LogicWorker
{
public:
	LogicWorker();
	~LogicWorker();

	void PostTask(std::shared_ptr<LogicNode> task);
	void Stop();

private:
	void TaskCallback(std::shared_ptr<LogicNode> task);
	void RegisterCallbacks();

	std::thread _work_thread;
	std::queue<std::shared_ptr<LogicNode>> _task_que;
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
	std::unordered_map<std::uint16_t, ResourceCallback> _handlers;
};
