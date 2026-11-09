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

	void PostTask(std::shared_ptr<LogicNode> task); //投递一帧到本线程
	void Stop(); //停止并join

private:
	void TaskCallback(std::shared_ptr<LogicNode> task); //取包体并查处理函数
	void RegisterCallbacks(); //建立消息ID到处理器的映射

	std::thread _work_thread;
	std::queue<std::shared_ptr<LogicNode>> _task_que; //只由本worker线程消费
	std::atomic<bool> _b_stop{false};
	std::mutex _mtx;
	std::condition_variable _cv;
	std::unordered_map<std::uint16_t, ResourceCallback> _handlers; //消息ID -> 处理函数
};
