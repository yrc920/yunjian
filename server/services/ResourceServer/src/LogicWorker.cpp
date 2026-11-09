#include "LogicWorker.h"
#include "ResourceHandler.h"
#include <iostream>

LogicWorker::LogicWorker()
{
	RegisterCallbacks();
	_work_thread = std::thread([this]() {
		while (!_b_stop.load()) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this] {
				return _b_stop.load() || !_task_que.empty();
			});

			if (_b_stop.load())
				break;

			auto task = _task_que.front();
			_task_que.pop();
			lock.unlock();

			TaskCallback(task);
		}
	});
}

LogicWorker::~LogicWorker()
{
	Stop();
}

void LogicWorker::PostTask(std::shared_ptr<LogicNode> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (_b_stop.load())
			return;
		_task_que.push(std::move(task));
	}
	_cv.notify_one();
}

void LogicWorker::Stop()
{
	if (_b_stop.exchange(true))
		return;
	_cv.notify_all();
	if (_work_thread.joinable())
		_work_thread.join();
}

void LogicWorker::TaskCallback(std::shared_ptr<LogicNode> task)
{
	auto iter = _handlers.find(task->_recvnode->_msg_id);
	if (iter == _handlers.end()) {
		std::cout << "no handler for msg id " << task->_recvnode->_msg_id << std::endl;
		return;
	}

	try {
		const std::string body(task->_recvnode->_data, task->_recvnode->_cur_len);
		iter->second(task->_session, task->_recvnode->_msg_id, body);
	}
	catch (std::exception& e) {
		std::cerr << "Exception in logic worker: " << e.what() << std::endl;
	}
}

void LogicWorker::RegisterCallbacks()
{
	_handlers[ID_UPLOAD_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleUpload(session, data, false);
	};
	_handlers[ID_UPLOAD_HEAD_ICON_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleUpload(session, data, true);
	};
	_handlers[ID_SYNC_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleSync(session, data);
	};
	_handlers[ID_DOWN_LOAD_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleDownload(session, data);
	};
}
