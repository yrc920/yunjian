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

	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateUserIcon(int uid, const std::string& icon);

private:
	MysqlMgr();
	MysqlDao _dao;
};
