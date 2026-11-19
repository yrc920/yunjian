#pragma once
// MySQL 连接池与借出句柄
//
// 借出即 RAII：Acquire() 返回的句柄在析构时自动归还，调用方不需要写归还语句。
// 归还时由池判断连接能否复用——deadline 取消过的会话标记为污染，直接丢弃并补建。
// 取连接前先过健康熔断，数据库整体不响应时立即失败而不是排队等满一个 deadline。

#include "MysqlSession.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct MysqlPoolOptions : MysqlConnectOptions {
	int pool_size = 5;
	std::string service_name; //日志前缀，如 ChatServer
};

//从 [Mysql] 段读连接参数：Host/Port/User/Passwd/Schema，可选 QueryTimeoutMs/PoolSize
MysqlPoolOptions MysqlPoolOptionsFromConfig(int default_pool_size, const std::string& service_name);

class MysqlPool;

//池里借出的一条连接：不可拷贝，析构即归还
class MysqlConnection {
public:
	MysqlConnection() = default;
	MysqlConnection(MysqlPool* pool, std::unique_ptr<MysqlSession> session);
	~MysqlConnection();

	MysqlConnection(const MysqlConnection&) = delete;
	MysqlConnection& operator=(const MysqlConnection&) = delete;
	MysqlConnection(MysqlConnection&& other) noexcept;
	MysqlConnection& operator=(MysqlConnection&& other) noexcept;

	explicit operator bool() const { return static_cast<bool>(_session); }
	MysqlSession& operator*() const { return *_session; }
	MysqlSession* operator->() const { return _session.get(); }
	MysqlSession* Get() const { return _session.get(); } //传给需要裸指针的辅助函数

private:
	void Reset();

	MysqlPool* _pool = nullptr;
	std::unique_ptr<MysqlSession> _session;
};

class MysqlPool {
public:
	explicit MysqlPool(MysqlPoolOptions options);
	~MysqlPool();

	MysqlPool(const MysqlPool&) = delete;
	MysqlPool& operator=(const MysqlPool&) = delete;

	//有界等待（1 秒）；熔断打开或池已停止时返回空句柄
	MysqlConnection Acquire();
	void Close();

private:
	//池内一格：会话本身 + 上次活跃时间（决定巡检时要不要 ping）
	struct Slot {
		std::unique_ptr<MysqlSession> session;
		long long last_oper_time = 0;
	};

	friend class MysqlConnection;
	void Return(std::unique_ptr<MysqlSession> session); //句柄析构时调用
	std::unique_ptr<Slot> CreateSlot();
	bool RefillOne(); //按需补一条，容量上限 pool_size
	void CheckLoop();
	void CheckConnections();

	MysqlPoolOptions _options;
	std::queue<std::unique_ptr<Slot>> _pool;
	std::mutex _mutex;
	std::condition_variable _cond;
	std::condition_variable _check_cond;
	int _connection_count = 0;
	bool _stopped = false;
	bool _check_requested = false;
	std::thread _check_thread;
};
