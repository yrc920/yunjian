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
