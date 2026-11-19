#pragma once
// 用户资料与资源访问（MySQL 经典协议，见 common/MysqlPool.h）
//

#include "MysqlPool.h"
#include "Singleton.h"
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include "data.h"

class MysqlSession;
class MysqlPool;

class MysqlDao : public Singleton<MysqlDao>
{
	friend class Singleton<MysqlDao>; //基类创建实例时需要访问私有构造函数
public:
	~MysqlDao();

	std::shared_ptr<UserInfo> GetUser(int uid); //按uid查用户资料
	bool UpdateUserIcon(int uid, const std::string& icon); //更新头像文件名
	//聊天图片全部落盘后回写消息状态：0 未上传，1 已上传；sender 用于确认归属
	bool UpdateChatMessageStatus(long long message_id, int sender, int status);
	//下载鉴权：本人、共用同一会话的成员，或对方当前头像
	bool CanAccessResource(int requester, int owner, const std::string& name);

private:
	MysqlDao(); //建立连接池；只由 Singleton 调用
	//连接池在首次 GetInstance 时建立，进程退出时关闭
	std::unique_ptr<MysqlPool> pool_; //共享给所有文件任务线程
};
