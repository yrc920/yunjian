#pragma once
// 逻辑系统：按会话分派到多个 LogicWorker，并保存上传进度
//

#include "Singleton.h"
#include "CSession.h"
#include "data.h"
#include "const.h"
#include <vector>
#include <memory>
#include <map>
#include <mutex>
#include <string>

class LogicWorker;

class LogicSystem : public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>;
public:
	~LogicSystem();

	void PostMsgToQue(std::shared_ptr<LogicNode> msg);
	void Stop();

	void AddFileInfo(std::shared_ptr<FileInfo> info);
	std::shared_ptr<FileInfo> GetFileInfo(const std::string& md5);
	//内存中没有时从Redis恢复，供暂停后换连接续传使用
	bool LoadFileInfo(const std::string& md5, std::shared_ptr<FileInfo>& info);
	void SaveFileInfo(const std::shared_ptr<FileInfo>& info);
	void RemoveFileInfo(const std::string& md5);

private:
	LogicSystem();

	std::vector<std::shared_ptr<LogicWorker>> _workers;
	std::map<std::string, std::shared_ptr<FileInfo>> _file_infos;
	std::mutex _file_mtx;
};
