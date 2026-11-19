#include "MysqlPool.h"
#include "ConfigMgr.h"
#include "Log.h"
#include "MysqlGuard.h"

#include <chrono>
#include <cstdlib>
#include <utility>

namespace {

long long NowSeconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace

MysqlPoolOptions MysqlPoolOptionsFromConfig(int default_pool_size, const std::string& service_name) {
	auto& cfg = ConfigMgr::Inst();
	MysqlPoolOptions options;
	options.host = cfg["Mysql"]["Host"];
	options.password = cfg["Mysql"]["Passwd"];
	options.schema = cfg["Mysql"]["Schema"];
	options.user = cfg["Mysql"]["User"];
	options.service_name = service_name;

	//经典协议端口，缺省 3306
	const std::string port_text = cfg["Mysql"]["Port"];
	const int port = port_text.empty() ? 3306 : std::atoi(port_text.c_str());
	options.port = port > 0 ? static_cast<unsigned>(port) : 3306;

	const std::string timeout_text = cfg["Mysql"]["QueryTimeoutMs"];
	const int timeout = timeout_text.empty() ? 5000 : std::atoi(timeout_text.c_str());
	options.query_timeout = std::chrono::milliseconds(timeout > 0 ? timeout : 5000);

	const std::string pool_text = cfg["Mysql"]["PoolSize"];
	const int pool_size = pool_text.empty() ? default_pool_size : std::atoi(pool_text.c_str());
	options.pool_size = pool_size > 0 ? pool_size : default_pool_size;

	if (options.host.empty())
		options.host = "127.0.0.1";
	if (options.user.empty())
		options.user = "root";
	return options;
}

// ---------------- MysqlConnection ----------------

MysqlConnection::MysqlConnection(MysqlPool* pool, std::unique_ptr<MysqlSession> session)
	: _pool(pool), _session(std::move(session)) {}

MysqlConnection::~MysqlConnection() {
	Reset();
}

MysqlConnection::MysqlConnection(MysqlConnection&& other) noexcept
	: _pool(other._pool), _session(std::move(other._session)) {
	other._pool = nullptr;
}

MysqlConnection& MysqlConnection::operator=(MysqlConnection&& other) noexcept {
	if (this != &other) {
		Reset();
		_pool = other._pool;
		_session = std::move(other._session);
		other._pool = nullptr;
	}
	return *this;
}

void MysqlConnection::Reset() {
	if (_session && _pool)
		_pool->Return(std::move(_session));
	_session.reset();
	_pool = nullptr;
}

// ---------------- MysqlPool ----------------

MysqlPool::MysqlPool(MysqlPoolOptions options) : _options(std::move(options)) {
	//先只建一条：数据库不可用时进程仍能启动，首个请求最多等一条连接的超时；
	//其余容量由巡检线程在后台补齐，不占用启动时间
	auto first = CreateSlot();
	if (first) {
		_pool.push(std::move(first));
		++_connection_count;
	}
	_check_requested = true; //构造函数返回后巡检线程立刻补齐
	LOG_INFO << "mysql pool ready service=" << _options.service_name
		<< " size=" << _connection_count << "/" << _options.pool_size
		<< " target=" << _options.host << ":" << _options.port
		<< " timeout=" << _options.query_timeout.count() << "ms";

	//熔断探针用独立会话，不占业务池；连不上或 ping 不通都抛异常交给 MysqlGuard
	MysqlGuard::Instance().Start(
		[options = _options, session = std::shared_ptr<MysqlSession>()]() mutable {
			if (!session || !session->IsOpen()) {
				session = std::make_shared<MysqlSession>(options);
				if (!session->Connect()) {
					session.reset();
					throw MysqlError("probe connect failed");
				}
			}
			if (!session->Ping()) {
				session.reset();
				throw MysqlError("probe ping failed");
			}
		},
		_options.service_name);

	_check_thread = std::thread([this]() { CheckLoop(); });
}

MysqlPool::~MysqlPool() {
	Close();
}

std::unique_ptr<MysqlPool::Slot> MysqlPool::CreateSlot() {
	auto session = std::make_unique<MysqlSession>(_options);
	if (!session->Connect())
		return nullptr;
	auto slot = std::make_unique<Slot>();
	slot->session = std::move(session);
	slot->last_oper_time = NowSeconds();
	return slot;
}

MysqlConnection MysqlPool::Acquire() {
	//熔断打开说明数据库当前不响应，直接失败，不要排进必然超时的等待队列
	if (!MysqlGuard::Instance().AllowQuery())
		return MysqlConnection();

	std::unique_lock<std::mutex> lock(_mutex);
	const bool ready = _cond.wait_for(lock, std::chrono::milliseconds(1000), [this] {
		return _stopped || !_pool.empty();
		});
	if (_stopped)
		return MysqlConnection();
	if (!ready) {
		//唤醒巡检线程尽快补池；数据库恢复后不必再等完整的 60 秒周期
		_check_requested = true;
		lock.unlock();
		_check_cond.notify_one();
		LOG_ERROR << "[mysql] pool wait timeout, no connection available";
		return MysqlConnection();
	}

	auto slot = std::move(_pool.front());
	_pool.pop();
	lock.unlock();
	return MysqlConnection(this, std::move(slot->session));
}

void MysqlPool::Return(std::unique_ptr<MysqlSession> session) {
	if (!session)
		return;

	std::unique_lock<std::mutex> lock(_mutex);
	if (_stopped)
		return; //停池后容量计数已清零，迟到的连接直接销毁，不再销账
	if (!session->IsOpen()) {
		//不可复用的连接不回队列：先按"空闲+借出"的总数销账，再请求巡检线程补建
		--_connection_count;
		_check_requested = true;
		lock.unlock();
		_check_cond.notify_one();
		return;
	}
	auto slot = std::make_unique<Slot>();
	slot->session = std::move(session);
	slot->last_oper_time = NowSeconds();
	_pool.push(std::move(slot));
	lock.unlock();
	_cond.notify_one();
}

void MysqlPool::Close() {
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stopped)
			return;
		_stopped = true;
	}
	_cond.notify_all();
	_check_cond.notify_all();
	if (_check_thread.joinable())
		_check_thread.join();
	MysqlGuard::Instance().Stop();

	std::lock_guard<std::mutex> lock(_mutex);
	while (!_pool.empty())
		_pool.pop();
	_connection_count = 0;
}

bool MysqlPool::RefillOne() {
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_stopped || _connection_count >= _options.pool_size)
			return false;
		++_connection_count; //先占位，避免并发补池突破上限
	}
	auto slot = CreateSlot();
	if (!slot) {
		std::lock_guard<std::mutex> lock(_mutex);
		--_connection_count;
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_pool.push(std::move(slot));
	}
	_cond.notify_one();
	return true;
}

void MysqlPool::CheckLoop() {
	while (true) {
		{
			std::unique_lock<std::mutex> lock(_mutex);
			_check_cond.wait_for(lock, std::chrono::seconds(60), [this] {
				return _stopped || _check_requested;
				});
			if (_stopped)
				return;
			_check_requested = false;
		}
		CheckConnections();
	}
}

void MysqlPool::CheckConnections() {
	//先按空闲连接逐条做轻量探测，空闲超过 5 秒才 ping，避免无谓往返
	size_t target = 0;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		target = _pool.size();
	}

	for (size_t i = 0; i < target; ++i) {
		std::unique_ptr<Slot> slot;
		{
			std::lock_guard<std::mutex> lock(_mutex);
			if (_pool.empty())
				break;
			slot = std::move(_pool.front());
			_pool.pop();
		}

		bool healthy = true;
		if (NowSeconds() - slot->last_oper_time >= 5) {
			healthy = slot->session && slot->session->Ping();
			if (healthy)
				slot->last_oper_time = NowSeconds();
		}

		if (healthy) {
			std::lock_guard<std::mutex> lock(_mutex);
			_pool.push(std::move(slot));
			continue;
		}

		//坏连接：销账并立刻补建，池容量不缩水
		{
			std::lock_guard<std::mutex> lock(_mutex);
			--_connection_count;
		}
		slot.reset();
		RefillOne();
	}

	//补齐容量缺口（初始建连失败或坏连接丢弃后留下的空位）
	int missing = 0;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		missing = _options.pool_size - _connection_count;
	}
	for (int i = 0; i < missing; ++i) {
		if (!RefillOne())
			break;
	}
}
