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
	MysqlDao();
	~MysqlDao();

	std::shared_ptr<UserInfo> GetUser(int uid);
	bool UpdateUserIcon(int uid, const std::string& icon);

private:
	std::unique_ptr<MySqlPool> pool_;
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
	MySqlPool(const std::string& host, unsigned port, const std::string& user,
		const std::string& pass, const std::string& schema, int pool_size);
	~MySqlPool();

	std::unique_ptr<SqlConnection> getConnection();
	void returnConnection(std::unique_ptr<SqlConnection> con);
	void Close();

private:
	void checkConnection();
	bool reconnect(long long timestamp);

	std::string _host;
	unsigned _port;
	std::string _user;
	std::string _pass;
	std::string _schema;
	int _pool_size;

	std::queue<std::unique_ptr<SqlConnection>> _pool;
	std::mutex _mutex;
	std::condition_variable _cond;
	std::condition_variable _check_cond;
	bool _check_requested{true};
	std::size_t _connection_count{0};
	std::atomic<bool> _stopped{false};
	std::thread _check_thread;
};
