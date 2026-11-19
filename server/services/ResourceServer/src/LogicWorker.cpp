#include "LogicWorker.h"
#include "ResourceHandler.h"
#include <iostream>
#include "const.h"
#include "Log.h"

LogicWorker::LogicWorker()
{
	RegisterCallbacks(); //先建消息表，再启动线程
	_work_thread = std::thread([this]() {
		while (!_b_stop.load()) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this] {
				return _b_stop.load() || !_task_que.empty(); //有消息或停服才醒
			});

			if (_b_stop.load())
				break;

			auto task = _task_que.front(); //取一帧
			_task_que.pop();
			lock.unlock(); //解析不占队列锁

			TaskCallback(task); //执行处理器
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
			return; //停服后不再收
		_task_que.push(std::move(task)); //转移所有权
	}
	_cv.notify_one();
}

void LogicWorker::Stop()
{
	if (_b_stop.exchange(true))
		return; //已经停过
	_cv.notify_all();
	if (_work_thread.joinable())
		_work_thread.join(); //等当前消息处理完
}

void LogicWorker::TaskCallback(std::shared_ptr<LogicNode> task)
{
	auto iter = _handlers.find(task->_recvnode->_msg_id); //按ID找处理器
	if (iter == _handlers.end()) {
		LOG_WARN << "no handler for msg id " << task->_recvnode->_msg_id;
		return;
	}

	try {
		const std::string body(task->_recvnode->_data, task->_recvnode->_cur_len); //只取实际长度
		iter->second(task->_session, task->_recvnode->_msg_id, body); //执行上传/同步/下载
	}
	catch (std::exception& e) { //异常不能逃出线程
		LOG_ERROR << "Exception in logic worker: " << e.what();
	}
}

void LogicWorker::RegisterCallbacks()
{
	_handlers[ID_UPLOAD_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleUpload(session, data, false); //通用文件
	};
	_handlers[ID_UPLOAD_HEAD_ICON_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleUpload(session, data, true); //头像，最后一片写user.icon
	};
	_handlers[ID_SYNC_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleSync(session, data); //暂停/重连后查进度
	};
	_handlers[ID_DOWN_LOAD_FILE_REQ] = [](std::shared_ptr<CSession> session,
		std::uint16_t, const std::string& data) {
		resource::HandleDownload(session, data); //按seq读一片
	};
}
