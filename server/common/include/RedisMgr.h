#pragma once
// Redis 连接池与命令封装（hiredis）
//
// 连接由进程内单例持有，池满或 Redis 不可达时 getConnection 有界等待后返回空，
// 调用方必须按「快速失败」处理，不能假设 Redis 一定可用。
//

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <hiredis/hiredis.h>

#include "Singleton.h"

//等待空闲连接的上限：Redis 卡住时业务线程必须在有限时间内失败
constexpr int REDIS_POOL_WAIT_MS = 1000;

class RedisMgr;

//连接池：固定容量，借出/归还由调用方成对完成
class RedisConPool {
public:
	RedisConPool(std::size_t pool_size, const std::string& host, int port, const std::string& pwd);
	~RedisConPool();

	RedisConPool(const RedisConPool&) = delete;
	RedisConPool& operator=(const RedisConPool&) = delete;

	redisContext* getConnection(); //有界等待；池已停止或超时返回 nullptr
	void returnConnection(redisContext* context); //连接有错时销毁并登记补池
	void Close(); //停止补池线程并释放空闲连接

private:
	bool connectOne(); //新建一条带认证的连接并放回池中
	void checkThread(); //周期性 PING 空闲连接并补齐容量

	std::atomic<bool> _stop{false};
	std::size_t _pool_size;
	std::string _host;
	std::string _pwd;
	int _port;

	std::deque<redisContext*> _idle;
	std::mutex _mutex;
	std::condition_variable _cond; //等待空闲连接
	std::condition_variable _check_cond; //唤醒补池线程

	std::size_t _connection_count = 0; //空闲 + 借出的连接总数
	bool _check_requested = true; //启动时先补齐未建成的连接
	std::thread _check_thread;
};

class RedisMgr : public Singleton<RedisMgr>
{
	friend class Singleton<RedisMgr>;
public:
	~RedisMgr();

	bool Ping(); //连通性探测，给健康检查用
	bool Get(const std::string& key, std::string& value); //读字符串，key 不存在返回 false
	bool Set(const std::string& key, const std::string& value);
	bool SetEx(const std::string& key, const std::string& value, int seconds); //SETEX，写入与过期原子完成
	bool Expire(const std::string& key, int seconds); //只改过期时间；key 不存在返回 false
	bool Del(const std::string& key);

	//自增并在首次写入时设置过期秒数，返回自增后的值，失败返回 -1
	long long IncrWithTtl(const std::string& key, int seconds);
	//SET NX EX：1 抢到 / 0 键已存在 / -1 执行失败
	int SetNxEx(const std::string& key, const std::string& value, int seconds);

	//执行 Lua 脚本；返回的 reply 由调用方 freeReplyObject，失败返回 nullptr
	redisReply* Eval(const std::string& script, const std::vector<std::string>& keys,
		const std::vector<std::string>& args);

	bool SRem(const std::string& key, const std::string& member);
	bool SMembers(const std::string& key, std::vector<std::string>& members);
	bool HGetAll(const std::string& key, std::map<std::string, std::string>& fields);

	void Close();

private:
	RedisMgr();

	std::shared_ptr<RedisConPool> _con_pool;
};
