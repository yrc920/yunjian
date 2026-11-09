#include "FileWorker.h"
#include "Base64.h"
#include "LogicSystem.h"
#include "MysqlMgr.h"
#include "RedisMgr.h"
#include "const.h"
#include <boost/filesystem.hpp>
#include <json/json.h>
#include <fstream>
#include <iostream>

FileWorker::FileWorker()
{
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

FileWorker::~FileWorker()
{
	Stop();
}

void FileWorker::PostTask(std::shared_ptr<FileTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (_b_stop.load())
			return;
		_task_que.push(std::move(task));
	}
	_cv.notify_one();
}

void FileWorker::Stop()
{
	if (_b_stop.exchange(true))
		return;
	_cv.notify_all();
	if (_work_thread.joinable())
		_work_thread.join();
}

void FileWorker::TaskCallback(std::shared_ptr<FileTask> task)
{
	std::string decoded;
	if (!base64::Decode(task->_data, decoded) || decoded.empty()) {
		task->_callback(ErrorCodes::FileDataInvalid);
		return;
	}

	//客户端按2048字节切片，偏移必须与seq对应，防止丢片后把文件写歪
	const long long expected_trans = static_cast<long long>(task->_seq - 1) * MAX_FILE_LEN
		+ static_cast<long long>(decoded.size());
	if (task->_seq < 1 || decoded.size() > MAX_FILE_LEN || expected_trans != task->_trans_size) {
		task->_callback(ErrorCodes::FileDataInvalid);
		return;
	}

	try {
		boost::filesystem::path file_path(task->_path);
		auto dir_path = file_path.parent_path();
		if (!dir_path.empty() && !boost::filesystem::exists(dir_path)) {
			boost::filesystem::create_directories(dir_path);
		}

		std::ios::openmode mode = std::ios::binary;
		mode |= (task->_seq == 1) ? std::ios::trunc : std::ios::app;
		std::ofstream outfile(task->_path, mode);
		if (!outfile) {
			std::cerr << "无法打开文件进行写入: " << task->_path << std::endl;
			task->_callback(ErrorCodes::FileWritePermissionFailed);
			return;
		}

		outfile.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
		if (!outfile) {
			std::cerr << "写入文件失败: " << task->_path << std::endl;
			task->_callback(ErrorCodes::FileWritePermissionFailed);
			return;
		}
		outfile.close();
	}
	catch (std::exception& e) {
		std::cerr << "FileWorker exception: " << e.what() << std::endl;
		task->_callback(ErrorCodes::FileWritePermissionFailed);
		return;
	}

	//写盘成功后再推进进度，同步接口返回的就是已经落盘的字节数
	auto file_info = LogicSystem::GetInstance()->GetFileInfo(task->_md5);
	if (file_info) {
		file_info->_seq = task->_seq;
		file_info->_trans_size = task->_trans_size;
		LogicSystem::GetInstance()->SaveFileInfo(file_info);
	}

	if (task->_last && task->_avatar) {
		UpdateAvatarInfo(task->_uid, task->_name);
	}

	task->_callback(ErrorCodes::Success);
}

//最后一片落盘后把头像文件名写回user表，并同步刷新用户名索引与基本信息缓存
void FileWorker::UpdateAvatarInfo(int uid, const std::string& name)
{
	if (!MysqlMgr::GetInstance()->UpdateUserIcon(uid, name)) {
		std::cerr << "update user icon failed, uid: " << uid << std::endl;
		return;
	}

	auto user_info = MysqlMgr::GetInstance()->GetUser(uid);
	if (!user_info) {
		return;
	}

	Json::Value base_root;
	base_root["uid"] = user_info->uid;
	base_root["name"] = user_info->name;
	base_root["email"] = user_info->email;
	base_root["nick"] = user_info->nick;
	base_root["desc"] = user_info->desc;
	base_root["sex"] = user_info->sex;
	base_root["icon"] = user_info->icon;
	RedisMgr::GetInstance()->Set(USER_BASE_INFO + std::to_string(uid),
		base_root.toStyledString());

	Json::Value name_root = base_root;
	RedisMgr::GetInstance()->Set(NAME_INFO + user_info->name, name_root.toStyledString());
}
