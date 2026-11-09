#include "RedisMgr.h"
#include "ConfigMgr.h"
#include <iostream>
#include <cstdlib>

namespace {
//命令失败时统一回收应答，避免每个分支重复写freeReplyObject
bool ReplyOk(redisReply* reply)
{
	if (reply == nullptr)
		return false;
	const bool ok = reply->type != REDIS_REPLY_ERROR;
	freeReplyObject(reply);
	return ok;
}
}

RedisMgr::RedisMgr()
{
	auto& cfg = ConfigMgr::Inst();
	_host = cfg["Redis"]["Host"];
	_port = std::atoi(cfg["Redis"]["Port"].c_str());
	if (_port <= 0)
		_port = 6379;
	_password = cfg["Redis"]["Passwd"];

	//预热几条连接，后续按需补足；启动时Redis不可用不阻止服务监听，首次使用会重试
	for (std::size_t i = 0; i < kPoolSize; ++i) {
		auto* context = Connect();
		if (context == nullptr)
			break;
		_idle.push(context);
		++_total;
	}
}

RedisMgr::~RedisMgr()
{
	Close();
}

redisContext* RedisMgr::Connect()
{
	timeval timeout{2, 0};
	redisContext* context = redisConnectWithTimeout(_host.c_str(), _port, timeout);
	if (context == nullptr || context->err) {
		if (context != nullptr) {
			std::cerr << "Redis connect error: " << context->errstr << std::endl;
			redisFree(context);
		}
		return nullptr;
	}

	if (!_password.empty()) {
		auto* reply = static_cast<redisReply*>(redisCommand(context, "AUTH %s", _password.c_str()));
		if (!ReplyOk(reply)) {
			std::cerr << "Redis auth failed" << std::endl;
			redisFree(context);
			return nullptr;
		}
	}
	return context;
}

redisContext* RedisMgr::Borrow()
{
	std::unique_lock<std::mutex> lock(_mtx);
	_cv.wait(lock, [this] {
		return _stopped.load() || !_idle.empty() || _total < kPoolSize;
	});
	if (_stopped.load())
		return nullptr;

	if (!_idle.empty()) {
		auto* context = _idle.front();
		_idle.pop();
		return context;
	}

	++_total;
	lock.unlock();
	auto* context = Connect();
	if (context == nullptr) {
		lock.lock();
		--_total;
		_cv.notify_one();
	}
	return context;
}

void RedisMgr::GiveBack(redisContext* context, bool healthy)
{
	if (context == nullptr)
		return;

	std::lock_guard<std::mutex> lock(_mtx);
	if (_stopped.load() || !healthy) {
		--_total;
		_cv.notify_one();
		redisFree(context);
		return;
	}
	_idle.push(context);
	_cv.notify_one();
}

bool RedisMgr::Get(const std::string& key, std::string& value)
{
	auto* context = Borrow();
	if (context == nullptr)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(context, "GET %s", key.c_str()));
	bool ok = reply != nullptr && reply->type == REDIS_REPLY_STRING;
	if (ok)
		value.assign(reply->str, reply->len);
	const bool healthy = reply != nullptr;
	if (reply != nullptr)
		freeReplyObject(reply);
	GiveBack(context, healthy);
	return ok;
}

bool RedisMgr::Set(const std::string& key, const std::string& value)
{
	auto* context = Borrow();
	if (context == nullptr)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(context, "SET %s %s",
		key.c_str(), value.c_str()));
	const bool healthy = reply != nullptr;
	const bool ok = ReplyOk(reply);
	GiveBack(context, healthy);
	return ok;
}

bool RedisMgr::SetExp(const std::string& key, const std::string& value, int expire_seconds)
{
	auto* context = Borrow();
	if (context == nullptr)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(context, "SETEX %s %d %s",
		key.c_str(), expire_seconds, value.c_str()));
	const bool healthy = reply != nullptr;
	const bool ok = ReplyOk(reply);
	GiveBack(context, healthy);
	return ok;
}

bool RedisMgr::Del(const std::string& key)
{
	auto* context = Borrow();
	if (context == nullptr)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(context, "DEL %s", key.c_str()));
	const bool healthy = reply != nullptr;
	const bool ok = ReplyOk(reply);
	GiveBack(context, healthy);
	return ok;
}

void RedisMgr::Close()
{
	if (_stopped.exchange(true))
		return;

	std::lock_guard<std::mutex> lock(_mtx);
	while (!_idle.empty()) {
		redisFree(_idle.front());
		_idle.pop();
		--_total;
	}
	_cv.notify_all();
}
