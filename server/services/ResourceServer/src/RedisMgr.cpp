#include "RedisMgr.h"
#include "ConfigMgr.h"
#include <iostream>
#include <cstdlib>

namespace {
bool ReplyOk(redisReply* reply)
{
	if (reply == nullptr) //命令没拿到回包，按失败处理
		return false;
	const bool ok = reply->type != REDIS_REPLY_ERROR; //只有ERROR算失败，NIL不算
	freeReplyObject(reply); //统一在这里释放，调用方不再持有reply
	return ok;
}
}

RedisMgr::RedisMgr()
{
	//启动时预热连接；Redis暂时不可用不阻止服务监听，Borrow时会按需重试
	auto& cfg = ConfigMgr::Inst();
	_host = cfg["Redis"]["Host"]; //与GateServer等共用同一实例，才能读到utoken
	_port = std::atoi(cfg["Redis"]["Port"].c_str()); //配置是字符串，非法值会得到0
	if (_port <= 0)
		_port = 6379; //缺省端口，本机联调由配置改成6380
	_password = cfg["Redis"]["Passwd"]; //AUTH用，留空表示无密码

	for (std::size_t i = 0; i < kPoolSize; ++i) {
		auto* context = Connect(); //逐条建连，每条都要单独AUTH
		if (context == nullptr)
			break; //连不上就停止预热，剩下的等Borrow按需补
		_idle.push(context); //放进空闲队列
		++_total; //已建连总数，Borrow据此判断还能不能新建
	}
}

RedisMgr::~RedisMgr()
{
	Close(); //停标志置位后由Close统一释放空闲连接
}

redisContext* RedisMgr::Connect()
{
	timeval timeout{2, 0}; //建连超时2秒，避免Redis挂掉时拖住服务启动
	redisContext* context = redisConnectWithTimeout(_host.c_str(), _port, timeout);
	if (context == nullptr || context->err) {
		if (context != nullptr) { //err非空时对象已经创建，必须显式释放
			std::cerr << "Redis connect error: " << context->errstr << std::endl;
			redisFree(context);
		}
		return nullptr; //失败交给Borrow重试，不抛异常
	}

	//密码只作用于这一条连接，池里每条都要单独AUTH
	if (!_password.empty()) {
		auto* reply = static_cast<redisReply*>(redisCommand(context, "AUTH %s", _password.c_str()));
		if (!ReplyOk(reply)) { //ReplyOk内部已释放reply
			std::cerr << "Redis auth failed" << std::endl;
			redisFree(context); //认证失败的连接不能进池
			return nullptr;
		}
	}
	return context; //已认证，可直接借出执行命令
}

redisContext* RedisMgr::Borrow()
{
	std::unique_lock<std::mutex> lock(_mtx); //保护空闲队列与总数
	_cv.wait(lock, [this] {
		return _stopped.load() || !_idle.empty() || _total < kPoolSize; //有连接或还能新建
	});
	if (_stopped.load())
		return nullptr; //停机后不再借出

	if (!_idle.empty()) { //优先复用空闲连接
		auto* context = _idle.front();
		_idle.pop();
		return context;
	}

	//先占住名额再解锁建连，失败时把名额还回去并唤醒下一个等待者
	++_total;
	lock.unlock(); //建连可能阻塞，不能占着池锁
	auto* context = Connect();
	if (context == nullptr) {
		lock.lock();
		--_total; //建连失败，归还名额
		_cv.notify_one();
	}
	return context;
}

void RedisMgr::GiveBack(redisContext* context, bool healthy)
{
	if (context == nullptr)
		return; //没借到就没什么可还

	std::lock_guard<std::mutex> lock(_mtx);
	if (_stopped.load() || !healthy) { //停机或连接已坏，直接释放
		--_total;
		_cv.notify_one();
		redisFree(context);
		return;
	}
	_idle.push(context); //健康连接回到池里
	_cv.notify_one();
}

bool RedisMgr::Get(const std::string& key, std::string& value)
{
	auto* context = Borrow(); //取一条连接
	if (context == nullptr)
		return false;

	auto* reply = static_cast<redisReply*>(redisCommand(context, "GET %s", key.c_str()));
	bool ok = reply != nullptr && reply->type == REDIS_REPLY_STRING; //只认字符串
	if (ok)
		value.assign(reply->str, reply->len); //保留二进制长度
	const bool healthy = reply != nullptr; //空reply说明连接已坏
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
	const bool ok = ReplyOk(reply); //只判断是否ERROR
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
	const bool ok = ReplyOk(reply); //SETEX成功返回OK
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
	const bool ok = ReplyOk(reply); //删除不存在的键也算成功
	GiveBack(context, healthy);
	return ok;
}

void RedisMgr::Close()
{
	if (_stopped.exchange(true))
		return; //只关闭一次

	std::lock_guard<std::mutex> lock(_mtx);
	while (!_idle.empty()) { //释放所有空闲连接
		redisFree(_idle.front());
		_idle.pop();
		--_total;
	}
	_cv.notify_all(); //唤醒还在等连接的线程，让它们返回空指针
}
