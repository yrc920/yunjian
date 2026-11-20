#include "MysqlDao.h"
#include "ConfigMgr.h"
#include <chrono>
#include "Log.h"

MysqlDao::MysqlDao()
{
	//连接参数从 [Mysql] 段读：Host/Port/User/Passwd/Schema，可选 QueryTimeoutMs/PoolSize
	//池大小 8：业务线程、gRPC 线程与资料推送共用
	pool_.reset(new MysqlPool(MysqlPoolOptionsFromConfig(8, "ChatServer")));
}

MysqlDao::~MysqlDao() {
	pool_->Close(); //关闭连接池, 释放资源
}

//更新本人可编辑的资料字段; 昵称与签名的长度由调用方按字符数校验
bool MysqlDao::UpdateUserProfile(int uid, const std::string& nick, int sex, const std::string& desc)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//desc 是MySQL保留字, 必须用反引号包起来; 三个字段一起写, 空串表示清空
		con->sql("UPDATE user SET nick = ?, sex = ?, `desc` = ? WHERE uid = ?")
			.bind(nick).bind(sex).bind(desc).bind(uid).execute();
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回空指针
	if (!con) {
		return nullptr;
	}
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏

	try {
		//执行查询语句(查询指定用户ID的用户信息, 显式列出列以便按索引取值)
		auto res = con->sql("SELECT uid, name, pwd, email, nick, `desc`, sex, icon "
			"FROM user WHERE uid = ?")
			.bind(uid).execute();

		std::shared_ptr<UserInfo> user_ptr = nullptr;
		auto row = res.fetchOne();
		//遍历结果集
		if (!row.isNull()) {
			//如果查询结果中有数据, 则说明用户ID存在, 将查询结果中的用户信息存储在user_ptr中, 返回user_ptr
			user_ptr.reset(new UserInfo);
			user_ptr->uid = row[0].get<int>();
			user_ptr->email = row[3].get<std::string>();
			user_ptr->name = row[1].get<std::string>();
			user_ptr->pwd = row[2].get<std::string>();
			user_ptr->nick = row[4].get<std::string>();
			user_ptr->desc = row[5].get<std::string>();
			user_ptr->sex = row[6].get<int>();
			user_ptr->icon = row[7].get<std::string>();
		}
		return user_ptr;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return nullptr;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(const std::string& name)
{
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回空指针
	if (!con) {
		return nullptr;
	}
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏

	try {
		//执行查询语句(查询指定用户名的用户信息, 与JDBC API时代一致, 不读取icon字段)
		auto res = con->sql("SELECT uid, name, pwd, email, nick, `desc`, sex, icon "
			"FROM user WHERE name = ?")
			.bind(name).execute();

		std::shared_ptr<UserInfo> user_ptr = nullptr;
		auto row = res.fetchOne();
		//遍历结果集
		if (!row.isNull()) {
			//如果查询结果中有数据, 则说明用户名存在, 将查询结果中的用户信息存储在user_ptr中, 返回user_ptr
			user_ptr.reset(new UserInfo);
			user_ptr->uid = row[0].get<int>();
			user_ptr->email = row[3].get<std::string>();
			//回填库里的规范写法：查询大小写不同时，缓存键与展示名仍保持一致
			user_ptr->name = row[1].get<std::string>();
			user_ptr->pwd = row[2].get<std::string>();
			user_ptr->nick = row[4].get<std::string>();
			user_ptr->desc = row[5].get<std::string>();
			user_ptr->sex = row[6].get<int>();
			user_ptr->icon = row[7].get<std::string>();
		}
		return user_ptr;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return nullptr;
	}
}
