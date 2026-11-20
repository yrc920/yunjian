#pragma once
// 数据库访问对象类, 用于封装对MySQL数据库的操作
//

#include "const.h"
#include "Singleton.h"
#include <thread>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
class MysqlPool; //MySQL连接池（common/MysqlPool.h）
struct UserInfo; //用户信息结构体, 包含用户名、密码、用户ID和邮箱(前置声明)

class MysqlDao : public Singleton<MysqlDao>
{
	friend class Singleton<MysqlDao>; //基类创建实例时需要访问私有构造函数
public:
	//注册用户, 返回用户ID, 如果注册失败则返回-1
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);
	bool UpdatePwd(const std::string& name, const std::string& newpwd); //更新密码
	//连通性探测：健康检查用，失败说明连接池或数据库不可用
	bool Ping();

	//检查用户名和邮箱是否匹配；db_error 区分"不匹配"与"数据库不可用"
	bool CheckEmail(const std::string& name, const std::string& email, bool& db_error);
	//检查用户名和密码是否匹配, 如果匹配则将用户信息存储在userInfo中。
	//db_error 区分"数据库不可用"与"密码错误"，前者不能计入登录失败次数
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo,
		bool& db_error);
	~MysqlDao();

private:
	MysqlDao(); //建立连接池；只由 Singleton 调用
	//MySQL连接池对象, 用于管理MySQL连接, 提供线程安全的获取和归还连接的接口
	//连接池在首次 GetInstance 时建立，进程退出时关闭
	std::unique_ptr<MysqlPool> pool_;
};

struct UserInfo {
	int uid; //用户ID
	std::string name; //用户名
	std::string email; //邮箱
	std::string pwd; //密码
};
