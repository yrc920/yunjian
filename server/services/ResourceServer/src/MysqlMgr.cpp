#include "MysqlMgr.h"

MysqlMgr::MysqlMgr() {}
MysqlMgr::~MysqlMgr() {}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid); //转给DAO，连接池由DAO管理
}

bool MysqlMgr::UpdateUserIcon(int uid, const std::string& icon)
{
	return _dao.UpdateUserIcon(uid, icon); //最后一片头像写回user.icon
}

bool MysqlMgr::UpdateChatMessageStatus(long long message_id, int sender, int status)
{
	return _dao.UpdateChatMessageStatus(message_id, sender, status); //图片资源完成后回写消息状态
}

bool MysqlMgr::CanAccessResource(int requester, int owner, const std::string& name)
{
	return _dao.CanAccessResource(requester, owner, name);
}
