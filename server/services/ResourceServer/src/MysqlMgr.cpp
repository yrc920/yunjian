#include "MysqlMgr.h"

MysqlMgr::MysqlMgr() {}
MysqlMgr::~MysqlMgr() {}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	return _dao.GetUser(uid);
}

bool MysqlMgr::UpdateUserIcon(int uid, const std::string& icon)
{
	return _dao.UpdateUserIcon(uid, icon);
}
