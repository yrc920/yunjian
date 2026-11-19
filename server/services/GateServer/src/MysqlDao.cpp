#include "MysqlDao.h"
#include "MysqlPool.h"
#include "ConfigMgr.h"
#include "PasswordHash.h"
#include <cstdlib>
#include <chrono>
#include <iostream>
#include "Log.h"

MysqlDao::MysqlDao()
{
	//连接参数从 [Mysql] 段读：Host/Port/User/Passwd/Schema，可选 QueryTimeoutMs/PoolSize
	pool_.reset(new MysqlPool(MysqlPoolOptionsFromConfig(8, "GateServer")));
}

MysqlDao::~MysqlDao() {
	pool_->Close(); //关闭连接池, 释放资源
}

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	//密码以哈希形式落库, 数据库中不出现明文。Argon2 是纯 CPU 计算（Release 约 40ms、
	//Debug 约 1s），放在借连接之前，不占着池连接算哈希
	const auto stored_pwd = PasswordHash::Hash(pwd);
	if (stored_pwd.empty())
		return -1;

	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	if (!con)
		return -1; //池等待超时或已停止
	try {
		//name 与 email 都有唯一键：先查一次给出明确结果，并发由唯一键兜底。
		//原实现依赖 reg_user 存储过程里的 user_id 计数表，多一次写和一张表
		auto exists = con->sql(
			"SELECT 1 FROM user WHERE name = ? OR email = ? LIMIT 1")
			.bind(name).bind(email).execute().fetchOne();
		if (!exists.isNull()) {
			return 0; //用户名或邮箱已存在
		}

		con->sql("INSERT INTO user (name, email, pwd) VALUES (?, ?, ?)")
			.bind(name).bind(email).bind(stored_pwd).execute();
		auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			return -1;
		}
		const int uid = id_row[0].get<int>();
		return uid;
	}
	catch (MysqlError& e) {
		//并发注册同一 name/email 时唯一键会拒绝，按"已存在"返回，不当成服务端故障
		const std::string message = e.what();
		if (message.find("Duplicate entry") != std::string::npos) {
			return 0;
		}
		LOG_ERROR << "mysql error: " << e.what();
		return -1;
	}
}

bool MysqlDao::Ping()
{
	auto con = pool_->Acquire();
	if (!con)
		return false; //池等待超时或已停止

	try {
		auto res = con->sql("SELECT 1").execute();
		auto row = res.fetchOne();
		return !row.isNull();
	}
	catch (MysqlError& e) {
		return false;
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email) {
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (!con)
			return false;

		//执行查询语句(查询指定用户名的邮箱地址)
		auto res = con->sql("SELECT email FROM user WHERE name = ?")
			.bind(name).execute();

		auto row = res.fetchOne();
		//用户名不存在时不能返回 true：否则 /reset_pwd 会给人"重置成功"的假象，
		//实际 UpdatePwd 影响 0 行。这里按"不匹配"处理，与密码错误同一个出口。
		if (row.isNull()) {
			return false;
		}
		auto email_in_db = row[0].get<std::string>();
		//如果查询结果中的email与输入的email不匹配, 则说明用户名和邮箱不匹配, 返回false
		return email == email_in_db;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& newpwd) {
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (!con)
			return false;

		//密码以哈希形式落库, 数据库中不出现明文
		auto stored_pwd = PasswordHash::Hash(newpwd);
		if (stored_pwd.empty()) {
			return false;
		}

		//执行更新语句(更新指定用户名的密码)
		auto res = con->sql("UPDATE user SET pwd = ? WHERE name = ?")
			.bind(stored_pwd).bind(name).execute();

		//用户名不存在时 affected rows 为 0，不能把"没人被更新"报成成功
		const bool updated = res.getAffectedItemsCount() > 0;
		return updated;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo,
	bool& db_error) {
	db_error = false;
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏

	try {
		//连接池超时或数据库不可用：这是服务端故障，不是密码错误
		if (!con) {
			db_error = true;
			return false;
		}

		//执行查询语句(查询指定邮箱的用户信息, 显式列出列以便按索引取值)
		auto res = con->sql("SELECT uid, name, pwd, email FROM user WHERE email = ?")
			.bind(email).execute();

		auto row = res.fetchOne();
		//未查到该邮箱对应的用户, 按密码不匹配处理
		if (row.isNull())
			return false;

		std::string origin_pwd = row[2].get<std::string>(); //第2列为pwd
		userInfo.name = row[1].get<std::string>();
		userInfo.email = email;
		userInfo.uid = row[0].get<int>();
		userInfo.pwd = origin_pwd;

		//库中 pwd 为 Argon2id 哈希串, 直接校验
		const bool valid = PasswordHash::Verify(pwd, origin_pwd);
		//旧账号仍可能是 256 MiB / 3 次的哈希：登录成功后顺手升级到当前参数，
		//只做一次额外写入；升级失败不影响本次登录
		if (valid && PasswordHash::NeedsRehash(origin_pwd)) {
			const auto upgraded = PasswordHash::Hash(pwd);
			if (!upgraded.empty()) {
				try {
					con->sql("UPDATE user SET pwd = ? WHERE uid = ?")
						.bind(upgraded).bind(userInfo.uid).execute();
				}
				catch (MysqlError& e) {
					LOG_ERROR << "password rehash update failed: " << e.what();
				}
			}
		}
		return valid;
	}
	catch (MysqlError& e) {
		//查询本身失败（连接断开、SQL 错误）属于服务端故障
		db_error = true;
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}
