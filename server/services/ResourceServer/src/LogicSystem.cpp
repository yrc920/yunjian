#include "LogicSystem.h"
#include "LogicWorker.h"
#include "RedisMgr.h"
#include <boost/filesystem.hpp>
#include <json/json.h>
#include <iostream>

LogicSystem::LogicSystem()
{
	//逻辑线程数固定：每个worker有自己的队列和线程，不与其他worker共享锁
	for (int i = 0; i < LOGIC_WORKER_COUNT; ++i)
		_workers.push_back(std::make_shared<LogicWorker>()); //启动即建线程
}

LogicSystem::~LogicSystem()
{
	Stop();
}

void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg)
{
	if (_workers.empty() || !msg || !msg->_session)
		return; //没有worker或消息不完整
	auto index = std::hash<std::string>{}(msg->_session->GetSessionId()) % _workers.size(); //同一会话固定同一线程
	_workers[index]->PostTask(std::move(msg)); //入该线程队列
}

void LogicSystem::Stop()
{
	for (auto& worker : _workers)
		worker->Stop(); //重复调用安全
}

void LogicSystem::AddFileInfo(std::shared_ptr<FileInfo> info)
{
	if (!info)
		return;
	std::lock_guard<std::mutex> lock(_file_mtx); //多个逻辑线程与文件线程共用
	_file_infos[info->_md5] = std::move(info); //seq=1时覆盖旧记录
}

std::shared_ptr<FileInfo> LogicSystem::GetFileInfo(const std::string& md5)
{
	std::lock_guard<std::mutex> lock(_file_mtx);
	auto iter = _file_infos.find(md5);
	if (iter == _file_infos.end())
		return nullptr; //内存里没有，调用方再走LoadFileInfo
	return iter->second;
}

bool LogicSystem::LoadFileInfo(const std::string& md5, std::shared_ptr<FileInfo>& info)
{
	{
		std::lock_guard<std::mutex> lock(_file_mtx);
		auto iter = _file_infos.find(md5);
		if (iter != _file_infos.end()) { //优先内存
			info = iter->second;
			return true;
		}
	}

	std::string value;
	if (!RedisMgr::GetInstance()->Get(FILE_INFO_PREFIX + md5, value))
		return false; //Redis也没有这次传输

	Json::Reader reader;
	Json::Value root;
	if (!reader.parse(value, root) || !root.isObject())
		return false; //坏数据按没有进度处理

	auto loaded = std::make_shared<FileInfo>();
	loaded->_md5 = md5; //键本身就是md5
	loaded->_file_path = root["file_path"].asString(); //首次上传时确定的路径
	loaded->_name = root["name"].asString(); //文件名
	loaded->_uid = root["uid"].asInt(); //归属用户
	loaded->_seq = root["seq"].asInt(); //最后落盘分片
	loaded->_accepted_seq = loaded->_seq; //从Redis恢复时只能从已落盘位置继续接收
	loaded->_total_size = root["total_size"].asInt64(); //总大小
	loaded->_trans_size = root["trans_size"].asInt64(); //已落盘大小
	loaded->_avatar = root["avatar"].asBool(); //是否头像

	//字段不完整说明 Redis 里是坏数据，按没有进度处理，客户端会从第一片重来
	if (loaded->_name.empty() || loaded->_file_path.empty() || loaded->_uid <= 0)
		return false;

	//进度只在每 16 片或每 1 MB 落一次 Redis，服务重启后可能落后于磁盘。
	//续传必须以实际文件长度为准：按完整分片对齐，崩溃留下的半个分片截掉，
	//否则客户端从旧偏移继续追加会把重复字节写进文件
	boost::system::error_code ec;
	const auto bytes = boost::filesystem::file_size(loaded->_file_path, ec);
	if (ec)
		return false; //Redis 有进度但磁盘没有文件，按全新上传处理（首片会截断重建）
	const long long complete_seq = static_cast<long long>(bytes) / MAX_FILE_LEN;
	const long long aligned = complete_seq * MAX_FILE_LEN;
	if (aligned != static_cast<long long>(bytes)) {
		boost::filesystem::resize_file(loaded->_file_path,
			static_cast<std::uintmax_t>(aligned), ec); //丢弃不完整分片
		if (ec)
			return false;
	}
	if (aligned > loaded->_trans_size) {
		loaded->_trans_size = aligned;
		loaded->_seq = static_cast<int>(complete_seq);
		loaded->_accepted_seq = loaded->_seq;
	}

	AddFileInfo(loaded); //写回内存，后续分片不再查Redis
	info = std::move(loaded);
	return true;
}

void LogicSystem::SaveFileInfo(const std::shared_ptr<FileInfo>& info)
{
	if (!info)
		return;

	Json::Value root;
	root["file_path"] = info->_file_path; //续传时沿用首次路径
	root["name"] = info->_name; //文件名
	root["uid"] = info->_uid; //归属用户
	root["seq"] = info->_seq; //已落盘分片
	root["total_size"] = Json::Int64(info->_total_size); //总大小
	root["trans_size"] = Json::Int64(info->_trans_size); //已落盘大小
	root["avatar"] = info->_avatar; //是否头像

	if (!RedisMgr::GetInstance()->SetEx(FILE_INFO_PREFIX + info->_md5,
		root.toStyledString(), FILE_INFO_EXPIRE)) { //一小时过期
		std::cerr << "save file progress to redis failed, md5: " << info->_md5 << std::endl;
	}

	AddFileInfo(info); //内存与Redis保持一致
}

void LogicSystem::RemoveFileInfo(const std::string& md5)
{
	std::lock_guard<std::mutex> lock(_file_mtx);
	_file_infos.erase(md5); //只删内存，Redis留作最后同步
}
