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

	void PostMsgToQue(std::shared_ptr<LogicNode> msg); //按会话ID分派到一个LogicWorker
	void Stop(); //停止并回收所有逻辑线程

	void AddFileInfo(std::shared_ptr<FileInfo> info); //登记一次上传的进度
	std::shared_ptr<FileInfo> GetFileInfo(const std::string& md5); //只查内存
	//内存中没有时从Redis恢复，供暂停后换连接续传使用
	bool LoadFileInfo(const std::string& md5, std::shared_ptr<FileInfo>& info);
	void SaveFileInfo(const std::shared_ptr<FileInfo>& info); //写Redis并更新内存
	void RemoveFileInfo(const std::string& md5); //完成后清内存，Redis保留一小时

private:
	LogicSystem();

	std::vector<std::shared_ptr<LogicWorker>> _workers; //固定数量，按会话哈希取
	std::map<std::string, std::shared_ptr<FileInfo>> _file_infos; //md5 -> 进度
	std::mutex _file_mtx; //多个LogicWorker与FileWorker共同访问
};
