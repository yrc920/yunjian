#include "LogicSystem.h"
#include "LogicWorker.h"
#include "RedisMgr.h"
#include <json/json.h>
#include <iostream>

LogicSystem::LogicSystem()
{
	for (int i = 0; i < LOGIC_WORKER_COUNT; ++i)
		_workers.push_back(std::make_shared<LogicWorker>());
}

LogicSystem::~LogicSystem()
{
	Stop();
}

//同一会话固定落到同一逻辑线程，避免同一连接的多片请求并发处理
void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg)
{
	if (_workers.empty() || !msg || !msg->_session)
		return;
	auto index = std::hash<std::string>{}(msg->_session->GetSessionId()) % _workers.size();
	_workers[index]->PostTask(std::move(msg));
}

void LogicSystem::Stop()
{
	for (auto& worker : _workers)
		worker->Stop();
}

void LogicSystem::AddFileInfo(std::shared_ptr<FileInfo> info)
{
	if (!info)
		return;
	std::lock_guard<std::mutex> lock(_file_mtx);
	_file_infos[info->_md5] = std::move(info);
}

std::shared_ptr<FileInfo> LogicSystem::GetFileInfo(const std::string& md5)
{
	std::lock_guard<std::mutex> lock(_file_mtx);
	auto iter = _file_infos.find(md5);
	if (iter == _file_infos.end())
		return nullptr;
	return iter->second;
}

bool LogicSystem::LoadFileInfo(const std::string& md5, std::shared_ptr<FileInfo>& info)
{
	{
		std::lock_guard<std::mutex> lock(_file_mtx);
		auto iter = _file_infos.find(md5);
		if (iter != _file_infos.end()) {
			info = iter->second;
			return true;
		}
	}

	std::string value;
	if (!RedisMgr::GetInstance()->Get(FILE_INFO_PREFIX + md5, value))
		return false;

	Json::Reader reader;
	Json::Value root;
	if (!reader.parse(value, root) || !root.isObject())
		return false;

	auto loaded = std::make_shared<FileInfo>();
	loaded->_md5 = md5;
	loaded->_file_path = root["file_path"].asString();
	loaded->_name = root["name"].asString();
	loaded->_uid = root["uid"].asInt();
	loaded->_seq = root["seq"].asInt();
	loaded->_total_size = root["total_size"].asInt64();
	loaded->_trans_size = root["trans_size"].asInt64();
	loaded->_avatar = root["avatar"].asBool();

	if (loaded->_name.empty() || loaded->_file_path.empty() || loaded->_uid <= 0)
		return false;

	AddFileInfo(loaded);
	info = std::move(loaded);
	return true;
}

void LogicSystem::SaveFileInfo(const std::shared_ptr<FileInfo>& info)
{
	if (!info)
		return;

	Json::Value root;
	root["file_path"] = info->_file_path;
	root["name"] = info->_name;
	root["uid"] = info->_uid;
	root["seq"] = info->_seq;
	root["total_size"] = Json::Int64(info->_total_size);
	root["trans_size"] = Json::Int64(info->_trans_size);
	root["avatar"] = info->_avatar;

	if (!RedisMgr::GetInstance()->SetExp(FILE_INFO_PREFIX + info->_md5,
		root.toStyledString(), FILE_INFO_EXPIRE)) {
		std::cerr << "save file progress to redis failed, md5: " << info->_md5 << std::endl;
	}

	AddFileInfo(info);
}

void LogicSystem::RemoveFileInfo(const std::string& md5)
{
	std::lock_guard<std::mutex> lock(_file_mtx);
	_file_infos.erase(md5);
}
