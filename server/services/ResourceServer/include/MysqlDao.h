#pragma once
// MySQL X DevAPI 连接池与用户资料访问
//

#include <mysqlx/xdevapi.h>
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <string>
#include "data.h"

class SqlConnection;
class MySqlPool;

class MysqlDao
{
public:
	MysqlDao(); //读取[Mysql]配置并初始化连接池
	~MysqlDao();

	std::shared_ptr<UserInfo> GetUser(int uid); //按uid查用户资料
	bool UpdateUserIcon(int uid, const std::string& icon); //更新头像文件名
	//聊天图片全部落盘后回写消息状态：0 未上传，1 已上传；sender 用于确认归属
	bool UpdateChatMessageStatus(long long message_id, int sender, int status);
	//下载鉴权：本人、共用同一会话的成员，或对方当前头像
	bool CanAccessResource(int requester, int owner, const std::string& name);

private:
	std::unique_ptr<MySqlPool> pool_; //共享给所有文件任务线程
};

//池内连接：X DevAPI 会话、最近活跃时间与可复用标记
class SqlConnection {
public:
	SqlConnection(std::unique_ptr<mysqlx::Session> session, int64_t last_time)
		: _session(std::move(session)), _last_oper_time(last_time) {}

	std::unique_ptr<mysqlx::Session> _session;
	int64_t _last_oper_time;
	bool _healthy{true};
};

class MySqlPool {
public:
	//按配置建pool_size条会话，并启动后台巡检线程
	MySqlPool(const std::string& host, unsigned port, const std::string& user,
		const std::string& pass, const std::string& schema, int pool_size);
	~MySqlPool();

	std::unique_ptr<SqlConnection> getConnection(); //借出一条连接，池空时等待
	void returnConnection(std::unique_ptr<SqlConnection> con); //归还连接，损坏的交给巡检线程重建
	void Close(); //停止巡检线程并释放池内全部连接

private:
	void checkConnection(); //后台巡检：补连、回收长时间空闲的连接
	bool reconnect(long long timestamp); //按时间阈值重建会话，成功才放回池中

	std::string _host; //MySQL地址
	unsigned _port; //X协议端口，默认33060
	std::string _user; //登录用户
	std::string _pass; //登录密码
	std::string _schema; //目标库
	int _pool_size; //期望维持的连接数

	std::queue<std::unique_ptr<SqlConnection>> _pool; //空闲连接
	std::mutex _mutex; //保护空闲队列、计数与停止标志
	std::condition_variable _cond; //借出等待
	std::condition_variable _check_cond; //巡检线程等待
	bool _check_requested{true}; //有连接损坏时请求立即巡检
	std::size_t _connection_count{0}; //当前存活连接数
	std::atomic<bool> _stopped{false}; //停机标志，阻止新的借出与归还
	std::thread _check_thread; //巡检线程
};
