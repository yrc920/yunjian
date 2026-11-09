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

private:
	MysqlMgr();
	MysqlDao _dao;
};
