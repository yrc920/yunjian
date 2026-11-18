#ifdef _WIN32
#include <winsock2.h> //MSVC 下 hiredis 只前置声明 timeval，完整定义由 winsock2 提供，且必须早于 windows.h
#endif
#include "RedisMgr.h"
#include "ConfigMgr.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#ifndef _WIN32
#include <sys/time.h>
#endif

namespace {
//建连与命令都要有上限：Redis 不可达时不能让业务线程永久阻塞
struct timeval ConnectTimeout()
{
	return timeval{ 1, 500000 };
}

struct timeval CommandTimeout()
{
	return timeval{ 2, 0 };
}

//借出的连接必须归还：所有命令实现共用一个守卫，避免每条命令手写 returnConnection
class BorrowedConnection
{
public:
	explicit BorrowedConnection(RedisConPool& pool)
		: _pool(pool), _context(pool.getConnection()) {}

	~BorrowedConnection()
	{
		if (_context != nullptr)
			_pool.returnConnection(_context);
	}

	BorrowedConnection(const BorrowedConnection&) = delete;
	BorrowedConnection& operator=(const BorrowedConnection&) = delete;

	redisContext* get() const { return _context; }
	explicit operator bool() const { return _context != nullptr; }

private:
	RedisConPool& _pool;
	redisContext* _context;
};

bool IsStatusOk(const redisReply* reply)
{
	return reply != nullptr && reply->type == REDIS_REPLY_STATUS && reply->str != nullptr
		&& (std::strcmp(reply->str, "OK") == 0 || std::strcmp(reply->str, "ok") == 0);
}
}

RedisConPool::RedisConPool(std::size_t pool_size, const std::string& host, int port,
	const std::string& pwd)
	: _pool_size(pool_size == 0 ? 1 : pool_size), _host(host), _pwd(pwd), _port(port)
{
	//启动时先尽力建满；建不成的连接由补池线程在 60 秒周期内继续尝试
	for (std::size_t i = 0; i < _pool_size; ++i) {
		if (!connectOne())
			break;
	}

	_check_thread = std::thread([this]() {
		while (!_stop.load()) {
			{
				std::unique_lock<std::mutex> lock(_mutex);
				//两个唤醒来源：停机，或业务线程归还坏连接后请求补池
				_check_cond.wait_for(lock, std::chrono::seconds(60),
					[this] { return _stop.load() || _check_requested; });
				if (_stop.load())
					break;
				_check_requested = false; //在锁内清除，本轮未补齐的缺口由本轮循环补足
			}
			checkThread();
		}
	});
}

RedisConPool::~RedisConPool()
{
	Close();
}

bool RedisConPool::connectOne()
{
	if (_stop.load())
		return false;

	auto* context = redisConnectWithTimeout(_host.c_str(), _port, ConnectTimeout());
	if (context == nullptr || context->err != 0) {
		if (context != nullptr)
			redisFree(context);
		return false;
	}
	redisSetTimeout(context, CommandTimeout());

	auto* reply = static_cast<redisReply*>(redisCommand(context, "AUTH %s", _pwd.c_str()));
	//只有收到 OK 才入池：密码错误、连接中断都会让 redisCommand 返回空或 ERROR
	if (!IsStatusOk(reply)) {
		freeReplyObject(reply);
		redisFree(context);
		return false;
	}
	freeReplyObject(reply);

	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop.load()) {
			redisFree(context);
			return false;
		}
		_idle.push_back(context);
		++_connection_count;
	}
	_cond.notify_one();
	return true;
}

void RedisConPool::checkThread()
{
	//一次只借出一条做 PING：全部借出会让并发取连接的线程空等，
	//而运行时改不了连接队列，只能保持窗口尽量小
	std::size_t pending;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		pending = _idle.size();
	}

	for (std::size_t i = 0; i < pending && !_stop.load(); ++i) {
		redisContext* context = nullptr;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_idle.empty())
				break;
			context = _idle.front();
			_idle.pop_front();
		}

		auto* reply = static_cast<redisReply*>(redisCommand(context, "PING"));
		const bool alive = context->err == 0 && reply != nullptr && reply->type != REDIS_REPLY_ERROR;
		freeReplyObject(reply);

		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (alive) {
				_idle.push_back(context); //健康连接原样放回
			}
			else {
				//死连接当场销毁并销账，只扣这一条：此刻借出的连接仍计入总数
				redisFree(context);
				if (_connection_count > 0) --_connection_count;
			}
		}
	}

	//补足初始连接失败、心跳失败或归还坏连接造成的容量缺口
	while (!_stop.load()) {
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_connection_count >= _pool_size)
				break;
		}
		if (!connectOne())
			break;
	}
}

redisContext* RedisConPool::getConnection()
{
	std::unique_lock<std::mutex> lock(_mutex);
	//有界等待：Redis 不可达或连接耗尽时，业务线程必须在有限时间内失败
	const bool ready = _cond.wait_for(lock, std::chrono::milliseconds(REDIS_POOL_WAIT_MS), [this] {
		return _stop.load() || !_idle.empty();
	});
	if (_stop.load())
		return nullptr;
	if (!ready) {
		//唤醒补池线程尽快重建；Redis 恢复后不必再等完整的巡检周期
		_check_requested = true;
		lock.unlock();
		_check_cond.notify_one();
		std::cerr << "[redis] no connection available, wait timed out" << std::endl;
		return nullptr;
	}

	auto* context = _idle.front();
	_idle.pop_front();
	return context;
}

void RedisConPool::returnConnection(redisContext* context)
{
	if (context == nullptr)
		return;

	bool request_check = false;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop.load()) {
			redisFree(context);
			if (_connection_count > 0) --_connection_count;
			return;
		}
		if (context->err != 0) {
			//不可复用的连接不回到队列：先销账并登记补池，通知放到锁外
			redisFree(context);
			if (_connection_count > 0) --_connection_count;
			_check_requested = true;
			request_check = true;
		}
		else {
			_idle.push_back(context);
			_cond.notify_one();
		}
	}
	if (request_check)
		_check_cond.notify_one();
}

void RedisConPool::Close()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stop.exchange(true))
			return; //重复 Close 只做一次
	}
	_cond.notify_all();
	_check_cond.notify_all();
	if (_check_thread.joinable())
		_check_thread.join();

	std::deque<redisContext*> pending;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		pending.swap(_idle);
		_connection_count = 0;
	}
	for (auto* context : pending)
		redisFree(context);
}

RedisMgr::RedisMgr()
{
	auto& cfg = ConfigMgr::Inst();
	const std::string pool_size = cfg.GetValue("Redis", "PoolSize");
	//默认 10 条：四个逻辑分片、gRPC 线程与状态上报同时借连接时不会排队
	const int size = pool_size.empty() ? 10 : std::atoi(pool_size.c_str());
	_con_pool = std::make_shared<RedisConPool>(
		static_cast<std::size_t>(size <= 0 ? 10 : size),
		cfg.GetValue("Redis", "Host"),
		std::atoi(cfg.GetValue("Redis", "Port").c_str()),
		cfg.GetValue("Redis", "Passwd"));
}

RedisMgr::~RedisMgr()
{
	Close();
}

bool RedisMgr::Get(const std::string& key, std::string& value)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "GET %s", key.c_str()));
	if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
		freeReplyObject(reply);
		return false;
	}
	value.assign(reply->str, reply->len); //按长度复制，值里出现 '\0' 也不截断
	freeReplyObject(reply);
	return true;
}

bool RedisMgr::Set(const std::string& key, const std::string& value)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "SET %s %b",
		key.c_str(), value.data(), value.size()));
	const bool ok = IsStatusOk(reply);
	freeReplyObject(reply);
	return ok;
}

bool RedisMgr::SetEx(const std::string& key, const std::string& value, int seconds)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	//SETEX 一条命令完成写入与设过期，不存在"写成功但没过期时间"的中间态
	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "SETEX %s %d %b",
		key.c_str(), seconds, value.data(), value.size()));
	const bool ok = IsStatusOk(reply);
	freeReplyObject(reply);
	return ok;
}

bool RedisMgr::Del(const std::string& key)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "DEL %s", key.c_str()));
	const bool ok = reply != nullptr && reply->type == REDIS_REPLY_INTEGER;
	freeReplyObject(reply);
	return ok;
}

long long RedisMgr::IncrWithTtl(const std::string& key, int seconds)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return -1;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "INCR %s", key.c_str()));
	if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
		freeReplyObject(reply);
		return -1;
	}
	const long long value = reply->integer;
	freeReplyObject(reply);

	//窗口键按时间桶命名，重复刷新 TTL 不会延长业务窗口，只保证陈旧键能自动回收
	auto* expire = static_cast<redisReply*>(redisCommand(con.get(), "EXPIRE %s %d",
		key.c_str(), seconds));
	freeReplyObject(expire);
	return value;
}

int RedisMgr::SetNxEx(const std::string& key, const std::string& value, int seconds)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return -1;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "SET %s %b NX EX %d",
		key.c_str(), value.data(), value.size(), seconds));
	if (reply == nullptr) {
		return -1;
	}

	int result = -1;
	if (IsStatusOk(reply))
		result = 1; //抢到
	else if (reply->type == REDIS_REPLY_NIL)
		result = 0; //键已存在

	freeReplyObject(reply);
	return result;
}

redisReply* RedisMgr::Eval(const std::string& script, const std::vector<std::string>& keys,
	const std::vector<std::string>& args)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return nullptr;

	//EVAL 参数按 脚本 / key 数量 / keys / args 拼接，全部走 Argv 以支持任意长度的值
	std::vector<const char*> argv;
	std::vector<std::size_t> argvlen;
	argv.push_back("EVAL"); argvlen.push_back(4);
	argv.push_back(script.c_str()); argvlen.push_back(script.size());
	const std::string key_count = std::to_string(keys.size());
	argv.push_back(key_count.c_str()); argvlen.push_back(key_count.size());
	for (const auto& key : keys) {
		argv.push_back(key.c_str());
		argvlen.push_back(key.size());
	}
	for (const auto& arg : args) {
		argv.push_back(arg.c_str());
		argvlen.push_back(arg.size());
	}

	return static_cast<redisReply*>(redisCommandArgv(con.get(),
		static_cast<int>(argv.size()), argv.data(), argvlen.data()));
}

bool RedisMgr::SRem(const std::string& key, const std::string& member)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "SREM %s %s",
		key.c_str(), member.c_str()));
	const bool ok = reply != nullptr && reply->type == REDIS_REPLY_INTEGER;
	freeReplyObject(reply);
	return ok;
}

bool RedisMgr::SMembers(const std::string& key, std::vector<std::string>& members)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "SMEMBERS %s", key.c_str()));
	const bool ok = reply != nullptr && reply->type == REDIS_REPLY_ARRAY;
	if (ok) {
		members.clear();
		members.reserve(reply->elements);
		for (std::size_t i = 0; i < reply->elements; ++i)
			members.emplace_back(reply->element[i]->str, reply->element[i]->len);
	}
	freeReplyObject(reply);
	return ok;
}

bool RedisMgr::HGetAll(const std::string& key, std::map<std::string, std::string>& fields)
{
	BorrowedConnection con(*_con_pool);
	if (!con)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(con.get(), "HGETALL %s", key.c_str()));
	//键不存在时返回空数组，对调用方等价于"没有这份数据"
	const bool ok = reply != nullptr && reply->type == REDIS_REPLY_ARRAY;
	if (ok) {
		fields.clear();
		for (std::size_t i = 0; i + 1 < reply->elements; i += 2)
			fields.emplace(std::string(reply->element[i]->str, reply->element[i]->len),
				std::string(reply->element[i + 1]->str, reply->element[i + 1]->len));
	}
	freeReplyObject(reply);
	return ok;
}

void RedisMgr::Close()
{
	if (_con_pool)
		_con_pool->Close();
}
