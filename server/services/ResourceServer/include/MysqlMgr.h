#pragma once
// MySQL 管理入口
//

#include "MysqlDao.h"
#include "Singleton.h"

class MysqlMgr : public Singleton<MysqlMgr>
{
	friend class Singleton<MysqlMgr>;
public:
	~MysqlMgr();

	std::shared_ptr<UserInfo> GetUser(int uid); //按uid取完整资料，用于刷新缓存
	bool UpdateUserIcon(int uid, const std::string& icon); //头像最后一片落盘后写user.icon
	//聊天图片最后一片落盘后把 chat_message.status 改成已上传（gRPC失败时的兜底）
	bool UpdateChatMessageStatus(long long message_id, int sender, int status);

private:
	MysqlMgr();
	MysqlDao _dao;
};
