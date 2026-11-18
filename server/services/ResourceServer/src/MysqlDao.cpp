#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "const.h"
#include <cstdlib>
#include <chrono>
#include <iostream>

namespace {
//所有池内会话统一带连接/读写超时：MySQL 被暂停或网络中断时，
//不能让文件线程永久挂住；超时后抛 mysqlx::Error，连接会被丢弃并补建
mysqlx::SessionSettings MakeSessionSettings(const std::string& host, unsigned port,
	const std::string& user, const std::string& pass, const std::string& schema)
{
	mysqlx::SessionSettings settings(host, port, user, pass, schema);
	//只设置建连超时；查询级超时见评审报告 Phase 1（DB 工作线程/看门狗）
	settings.set(mysqlx::SessionOption::CONNECT_TIMEOUT, std::chrono::milliseconds(2000));
	return settings;
}
}

MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst(); //配置按当前工作目录的config.ini读取
	const auto& host = cfg["Mysql"]["Host"]; //mysqlx走X协议，端口与SQL协议不同
	const auto& xport = cfg["Mysql"]["XPort"]; //官方MySQL的X插件默认监听33060
	const auto& user = cfg["Mysql"]["User"];
	const auto& pass = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"]; //库里只存头像文件名，图片落在Storage目录

	unsigned xport_num = 33060; //配置缺失时退回X插件默认端口
	if (!xport.empty()) {
		int value = std::atoi(xport.c_str()); //配置项是字符串，非法值会得到0
		if (value > 0)
			xport_num = static_cast<unsigned>(value); //只接受正数端口
	}
	pool_.reset(new MySqlPool(host, xport_num, user, pass, schema, 6)); //6条长连接由后台线程巡检
}

MysqlDao::~MysqlDao()
{
	pool_->Close(); //先停巡检线程再释放连接，避免析构时还有线程在借连接
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->getConnection(); //从池里借一条会话
	if (con == nullptr)
		return nullptr; //连接池已停

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con)); //任何分支退出都归还连接
	});

	try {
		auto res = con->_session->sql(
			"SELECT uid, name, pwd, email, nick, `desc`, sex, icon FROM user WHERE uid = ?") //只查需要的列
			.bind(uid).execute();
		auto row = res.fetchOne(); //按uid查询最多一行
		if (row.isNull())
			return nullptr; //用户不存在

		auto info = std::make_shared<UserInfo>(); //按列顺序回填
		info->uid = row[0].get<int>();
		info->name = row[1].get<std::string>();
		info->pwd = row[2].get<std::string>(); //缓存刷新不返回给客户端
		info->email = row[3].get<std::string>();
		info->nick = row[4].get<std::string>();
		info->desc = row[5].get<std::string>();
		info->sex = row[6].get<int>();
		info->icon = row[7].get<std::string>(); //刚写入的头像文件名
		return info;
	}
	catch (mysqlx::Error& e) {
		con->_healthy = false; //坏连接不回到池里
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
		pool_->returnConnection(std::move(con)); //RAII归还
	});

	try {
		con->_session->sql("UPDATE user SET icon = ? WHERE uid = ?") //只改头像字段
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

bool MysqlDao::CanAccessResource(int requester, int owner, const std::string& name)
{
	if (requester <= 0 || owner <= 0 || name.empty())
		return false;
	if (requester == owner)
		return true; //自己的文件不用再查

	auto con = pool_->getConnection(); //从池里借一条会话
	if (con == nullptr)
		return false; //池不可用时按拒绝处理：宁可下载失败，也不越权放行

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});

	try {
		//头像是公开资源：只要这个文件仍是对方的当前头像，任何登录用户都能取
		auto icon = con->_session->sql("SELECT 1 FROM user WHERE uid = ? AND icon = ?")
			.bind(owner).bind(name).execute().fetchOne();
		if (!icon.isNull())
			return true;

		//其余资源要求双方至少共处一个会话：单聊两人各一行成员记录，群聊同 thread_id
		auto shared = con->_session->sql(
			"SELECT 1 FROM conversation_member mine "
			"JOIN conversation_member peer ON peer.thread_id = mine.thread_id "
			"WHERE mine.uid = ? AND peer.uid = ? LIMIT 1")
			.bind(requester).bind(owner).execute().fetchOne();
		return !shared.isNull();
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //连接不可复用，归还时丢弃并补建
		std::cerr << "Mysqlx Error checking resource access: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::UpdateChatMessageStatus(long long message_id, int sender, int status)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
	});

	try {
		//gRPC 不可用时的兜底路径，同样带上 sender 限定
		con->_session->sql("UPDATE chat_message SET status = ? WHERE message_id = ? AND sender_id = ?")
			.bind(status).bind(static_cast<int64_t>(message_id)).bind(sender).execute();
		return true;
	}
	catch (mysqlx::Error& e) {
		con->_healthy = false;
		std::cerr << "Mysqlx Error in UpdateChatMessageStatus: " << e.what() << std::endl;
		return false;
	}
}

MySqlPool::MySqlPool(const std::string& host, unsigned port, const std::string& user,
	const std::string& pass, const std::string& schema, int pool_size)
	: _host(host), _port(port), _user(user), _pass(pass), _schema(schema), _pool_size(pool_size)
{
	for (int i = 0; i < _pool_size; ++i) {
		try {
			auto settings = MakeSessionSettings(_host, _port, _user, _pass, _schema); //X协议连接参数
			auto session = std::make_unique<mysqlx::Session>(settings);
			auto now = std::chrono::system_clock::now().time_since_epoch();
			auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();
			_pool.push(std::make_unique<SqlConnection>(std::move(session), timestamp)); //入池
			++_connection_count; //池内总数与队列同步记账
		}
		catch (mysqlx::Error& e) { //单条失败不阻止服务启动
			std::cout << "mysql connection init failed: " << e.what() << std::endl;
		}
	}

	_check_thread = std::thread([this]() { //周期检测线程
		while (!_stopped.load()) {
			{
				std::unique_lock<std::mutex> lock(_mutex);
				_check_cond.wait_for(lock, std::chrono::seconds(60), [this] {
					return _stopped.load() || _check_requested; //停服或有补池请求
				});
				if (_stopped.load())
					break;
				_check_requested = false; //在锁内清标记
			}
			checkConnection(); //心跳并按容量补建
		}
	});
}

void MySqlPool::checkConnection()
{
	std::size_t target = 0;
	{
		std::lock_guard<std::mutex> guard(_mutex);
		target = _pool.size(); //只检查当前空闲连接
	}

	auto now = std::chrono::system_clock::now().time_since_epoch();
	auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();

	for (std::size_t i = 0; i < target; ++i) {
		std::unique_ptr<SqlConnection> con;
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_pool.empty())
				break;
			con = std::move(_pool.front()); //借出检查
			_pool.pop();
		}

		bool healthy = true;
		if (timestamp - con->_last_oper_time >= 5) { //空闲超过5秒才心跳
			try {
				con->_session->sql("SELECT 1").execute(); //轻量查询保活
				con->_last_oper_time = timestamp;
			}
			catch (mysqlx::Error& e) {
				std::cout << "Error keeping connection alive: " << e.what() << std::endl;
				healthy = false; //坏连接丢弃
			}
		}

		std::lock_guard<std::mutex> guard(_mutex);
		if (healthy && !_stopped.load()) {
			_pool.push(std::move(con)); //健康连接回池
			_cond.notify_one();
		}
		else {
			--_connection_count; //坏连接销账，等下补建
		}
	}

	while (!_stopped.load()) {
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_connection_count >= static_cast<std::size_t>(_pool_size))
				break; //已满
		}
		if (!reconnect(timestamp))
			break; //数据库不可达时留到下个周期
	}
}

bool MySqlPool::reconnect(long long timestamp)
{
	try {
		auto settings = MakeSessionSettings(_host, _port, _user, _pass, _schema); //重新建会话
		auto session = std::make_unique<mysqlx::Session>(settings);
		auto con = std::make_unique<SqlConnection>(std::move(session), timestamp);
		{
			std::lock_guard<std::mutex> guard(_mutex);
			if (_stopped.load())
				return false; //停服后不再补建
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
	std::unique_lock<std::mutex> lock(_mutex); //保护空闲队列与停止标志
	//有界等待：数据库不可达或连接耗尽时，文件线程必须在有限时间内失败
	const bool ready = _cond.wait_for(lock, std::chrono::milliseconds(MYSQL_POOL_WAIT_MS), [this] {
		return _stopped.load() || !_pool.empty();
	});
	if (_stopped.load()) //停机期间不再借出连接
		return nullptr;
	if (!ready) {
		//唤醒检查线程尽快补池；DB 恢复后不必再等完整的巡检周期
		_check_requested = true;
		lock.unlock();
		_check_cond.notify_one();
		std::cerr << "[mysql] pool wait timeout, no connection available" << std::endl;
		return nullptr;
	}

	auto con = std::move(_pool.front()); //所有权交给调用方
	_pool.pop(); //出队后总数不变，归还时再push回去
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
	if (!con) //空指针直接丢弃，由unique_ptr自行析构
		return;

	std::unique_lock<std::mutex> lock(_mutex); //与借出、巡检共用同一把锁
	if (_stopped.load()) //停机后连接随unique_ptr析构释放
		return;

	//执行中报错或session已断的连接不能回池，交给检查线程重建
	if (!con->_healthy || !con->_session) {
		--_connection_count; //先减计数，检查线程才会补一条新的
		_check_requested = true; //标记需要重建，避免无谓轮询
		lock.unlock(); //先解锁再通知，减少唤醒后的抢锁
		_check_cond.notify_one();
		return;
	}

	//刷新最近使用时间，检查线程据此判断连接是否长时间空闲
	auto now = std::chrono::system_clock::now().time_since_epoch();
	con->_last_oper_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	_pool.push(std::move(con)); //健康连接回池复用
	_cond.notify_one(); //唤醒一个正在等待借出的线程
}

void MySqlPool::Close()
{
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_stopped = true; //先置停止位，阻止新的借出与归还
	}
	_cond.notify_all(); //唤醒所有等待者，让getConnection能退出wait
	_check_cond.notify_all(); //巡检线程也要退出
	if (_check_thread.joinable())
		_check_thread.join(); //等巡检退出后再清池子，避免并发访问

	std::lock_guard<std::mutex> lock(_mutex); //重新加锁保护剩余连接
	while (!_pool.empty())
		_pool.pop(); //unique_ptr逐个析构，session随之关闭
	_connection_count = 0; //计数归零，Close可重复调用
}

MySqlPool::~MySqlPool()
{
	Close(); //析构走同一套停机流程，保证线程先退出再释放资源
}
