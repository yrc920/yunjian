#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "const.h"
#include <cstdlib>
#include <chrono>
#include <iostream>

MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst();
	const auto& host = cfg["Mysql"]["Host"];
	const auto& xport = cfg["Mysql"]["XPort"];
	const auto& user = cfg["Mysql"]["User"];
	const auto& pass = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];

	unsigned xport_num = 33060;
	if (!xport.empty()) {
		int value = std::atoi(xport.c_str());
		if (value > 0)
			xport_num = static_cast<unsigned>(value);
	}
	pool_.reset(new MySqlPool(host, xport_num, user, pass, schema, 4));
}

MysqlDao::~MysqlDao()
{
	pool_->Close();
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return nullptr;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});

	try {
		auto res = con->_session->sql(
			"SELECT uid, name, pwd, email, nick, `desc`, sex, icon FROM user WHERE uid = ?")
			.bind(uid).execute();
		auto row = res.fetchOne();
		if (row.isNull())
			return nullptr;

		auto info = std::make_shared<UserInfo>();
		info->uid = row[0].get<int>();
		info->name = row[1].get<std::string>();
		info->pwd = row[2].get<std::string>();
		info->email = row[3].get<std::string>();
		info->nick = row[4].get<std::string>();
		info->desc = row[5].get<std::string>();
		info->sex = row[6].get<int>();
		info->icon = row[7].get<std::string>();
		return info;
	}
	catch (mysqlx::Error& e) {
		con->_healthy = false;
		std::cerr << "Mysqlx Error in GetUser: " << e.what() << std::endl;
		return nullptr;
	}
}

bool MysqlDao::UpdateUserIcon(int uid, const std::string& icon)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});

	try {
		con->_session->sql("UPDATE user SET icon = ? WHERE uid = ?")
			.bind(icon).bind(uid).execute();
		//同一个头像重复提交时affected rows为0，但语句本身成功，按成功返回
		return true;
	}
	catch (mysqlx::Error& e) {
		con->_healthy = false;
		std::cerr << "Mysqlx Error in UpdateUserIcon: " << e.what() << std::endl;
		return false;
	}
}

MySqlPool::MySqlPool(const std::string& host, unsigned port, const std::string& user,
	const std::string& pass, const std::string& schema, int pool_size)
	: _host(host), _port(port), _user(user), _pass(pass), _schema(schema), _pool_size(pool_size)
{
	for (int i = 0; i < _pool_size; ++i) {
		try {
			mysqlx::SessionSettings settings(_host, _port, _user, _pass, _schema);
			auto session = std::make_unique<mysqlx::Session>(settings);
			auto now = std::chrono::system_clock::now().time_since_epoch();
			auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
			_pool.push(std::make_unique<SqlConnection>(std::move(session), timestamp));
			++_connection_count;
		}
		catch (mysqlx::Error& e) {
			std::cout << "mysql connection init failed: " << e.what() << std::endl;
		}
	}

	_check_thread = std::thread([this]() {
		while (!_stopped.load()) {
			{
				std::unique_lock<std::mutex> lock(_mutex);
				_check_cond.wait_for(lock, std::chrono::seconds(60), [this] {
					return _stopped.load() || _check_requested;
				});
				if (_stopped.load())
					break;
				_check_requested = false;
			}
			checkConnection();
		}
	});
}

void MySqlPool::checkConnection()
{
	std::size_t target = 0;
	{
		std::lock_guard<std::mutex> guard(_mutex);
		target = _pool.size();
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();

	for (std::size_t i = 0; i < target; ++i) {
		std::unique_ptr<SqlConnection> con;
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_pool.empty())
				break;
			con = std::move(_pool.front());
			_pool.pop();
		}

		bool healthy = true;
		if (timestamp - con->_last_oper_time >= 5) {
			try {
				con->_session->sql("SELECT 1").execute();
				con->_last_oper_time = timestamp;
			}
			catch (mysqlx::Error& e) {
				std::cout << "Error keeping connection alive: " << e.what() << std::endl;
				healthy = false;
			}
		}

		std::lock_guard<std::mutex> guard(_mutex);
		if (healthy && !_stopped.load()) {
			_pool.push(std::move(con));
			_cond.notify_one();
		}
		else {
			--_connection_count;
		}
	}

	while (!_stopped.load()) {
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_connection_count >= static_cast<std::size_t>(_pool_size))
				break;
		}
		if (!reconnect(timestamp))
			break;
	}
}

bool MySqlPool::reconnect(long long timestamp)
{
	try {
		mysqlx::SessionSettings settings(_host, _port, _user, _pass, _schema);
		auto session = std::make_unique<mysqlx::Session>(settings);
		auto con = std::make_unique<SqlConnection>(std::move(session), timestamp);
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_stopped.load())
				return false;
			_pool.push(std::move(con));
			++_connection_count;
		}
		_cond.notify_one();
		return true;
	}
	catch (mysqlx::Error& e) {
		std::cout << "Reconnect failed, error is " << e.what() << std::endl;
		return false;
	}
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection()
{
	std::unique_lock<std::mutex> lock(_mutex);
	_cond.wait(lock, [this] {
		return _stopped.load() || !_pool.empty();
	});
	if (_stopped.load())
		return nullptr;

	auto con = std::move(_pool.front());
	_pool.pop();
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
	if (!con)
		return;

	std::unique_lock<std::mutex> lock(_mutex);
	if (_stopped.load())
		return;

	if (!con->_healthy || !con->_session) {
		--_connection_count;
		_check_requested = true;
		lock.unlock();
		_check_cond.notify_one();
		return;
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	con->_last_oper_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	_pool.push(std::move(con));
	_cond.notify_one();
}

void MySqlPool::Close()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stopped = true;
	}
	_cond.notify_all();
	_check_cond.notify_all();
	if (_check_thread.joinable())
		_check_thread.join();

	std::lock_guard<std::mutex> lock(_mutex);
	while (!_pool.empty())
		_pool.pop();
	_connection_count = 0;
}

MySqlPool::~MySqlPool()
{
	Close();
}
