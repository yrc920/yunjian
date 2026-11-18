#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "PasswordHash.h"
#include <cstdlib>
#include <chrono>
#include <iostream>

namespace {
//池内会话统一带建连超时：MySQL 不可达时，新建连接不能无限等待。
//注意：连接器 9.x 的 X DevAPI 在服务端被冻结/黑洞时不会按 READ_TIMEOUT 中断已建立的读，
//查询级超时依赖 Phase 1 把数据库调用移出 IO 线程（见 docs/backend-optimization-review.md）
mysqlx::SessionSettings MakeSessionSettings(const std::string& host, unsigned port,
	const std::string& user, const std::string& pass, const std::string& schema)
{
	mysqlx::SessionSettings settings(host, port, user, pass, schema);
	settings.set(mysqlx::SessionOption::CONNECT_TIMEOUT, std::chrono::milliseconds(2000));
	return settings;
}
}

MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst(); //获取配置管理器实例
	const auto& host = cfg["Mysql"]["Host"];
	const auto& xport = cfg["Mysql"]["XPort"]; //X Protocol端口号
	const auto& pwd = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];
	const auto& user = cfg["Mysql"]["User"];
	//X Protocol默认端口为33060, 若配置缺失则使用默认值
	unsigned xport_num = 33060;
	if (!xport.empty()) {
		int v = std::atoi(xport.c_str());
		if (v > 0) {
			xport_num = static_cast<unsigned>(v);
		}
	}
	//初始化MySQL连接池对象, 连接池大小为8: HTTP handler 并发执行密码哈希后的查询
	pool_.reset(new MySqlPool(host, xport_num, user, pwd, schema, 8));
}

MysqlDao::~MysqlDao() {
	pool_->Close(); //关闭连接池, 释放资源
}

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回-1
		if (con == nullptr)
			return -1;

		//密码以哈希形式落库, 数据库中不出现明文
		auto stored_pwd = PasswordHash::Hash(pwd);
		if (stored_pwd.empty()) {
			pool_->returnConnection(std::move(con));
			return -1;
		}

		//name 与 email 都有唯一键：先查一次给出明确结果，并发由唯一键兜底。
		//原实现依赖 reg_user 存储过程里的 user_id 计数表，多一次写和一张表
		auto exists = con->_session->sql(
			"SELECT 1 FROM user WHERE name = ? OR email = ? LIMIT 1")
			.bind(name).bind(email).execute().fetchOne();
		if (!exists.isNull()) {
			pool_->returnConnection(std::move(con));
			return 0; //用户名或邮箱已存在
		}

		con->_session->sql("INSERT INTO user (name, email, pwd) VALUES (?, ?, ?)")
			.bind(name).bind(email).bind(stored_pwd).execute();
		auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			pool_->returnConnection(std::move(con));
			return -1;
		}
		const int uid = id_row[0].get<int>();
		pool_->returnConnection(std::move(con));
		return uid;
	}
	catch (mysqlx::Error& e) {
		//并发注册同一 name/email 时唯一键会拒绝，按"已存在"返回，不当成服务端故障
		const std::string message = e.what();
		if (message.find("Duplicate entry") != std::string::npos) {
			pool_->returnConnection(std::move(con));
			return 0;
		}
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return -1;
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email) {
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (con == nullptr)
			return false;

		//执行查询语句(查询指定用户名的邮箱地址)
		auto res = con->_session->sql("SELECT email FROM user WHERE name = ?")
			.bind(name).execute();

		auto row = res.fetchOne();
		//用户名不存在时不能返回 true：否则 /reset_pwd 会给人"重置成功"的假象，
		//实际 UpdatePwd 影响 0 行。这里按"不匹配"处理，与密码错误同一个出口。
		if (row.isNull()) {
			pool_->returnConnection(std::move(con));
			return false;
		}
		auto email_in_db = row[0].get<std::string>();
		//如果查询结果中的email与输入的email不匹配, 则说明用户名和邮箱不匹配, 返回false
		pool_->returnConnection(std::move(con));
		return email == email_in_db;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& newpwd) {
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (con == nullptr)
			return false;

		//密码以哈希形式落库, 数据库中不出现明文
		auto stored_pwd = PasswordHash::Hash(newpwd);
		if (stored_pwd.empty()) {
			pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
			return false;
		}

		//执行更新语句(更新指定用户名的密码)
		auto res = con->_session->sql("UPDATE user SET pwd = ? WHERE name = ?")
			.bind(stored_pwd).bind(name).execute();

		//用户名不存在时 affected rows 为 0，不能把"没人被更新"报成成功
		const bool updated = res.getAffectedItemsCount() > 0;
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		return updated;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo,
	bool& db_error) {
	db_error = false;
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		});

	try {
		//连接池超时或数据库不可用：这是服务端故障，不是密码错误
		if (con == nullptr) {
			db_error = true;
			return false;
		}

		//执行查询语句(查询指定邮箱的用户信息, 显式列出列以便按索引取值)
		auto res = con->_session->sql("SELECT uid, name, pwd, email FROM user WHERE email = ?")
			.bind(email).execute();

		auto row = res.fetchOne();
		//未查到该邮箱对应的用户, 按密码不匹配处理
		if (row.isNull())
			return false;

		std::string origin_pwd = row[2].get<std::string>(); //第2列为pwd
		userInfo.name = row[1].get<std::string>();
		userInfo.email = email;
		userInfo.uid = row[0].get<int>();
		userInfo.pwd = origin_pwd;

		//库中 pwd 为 Argon2id 哈希串, 直接校验
		const bool valid = PasswordHash::Verify(pwd, origin_pwd);
		//旧账号仍可能是 256 MiB / 3 次的哈希：登录成功后顺手升级到当前参数，
		//只做一次额外写入；升级失败不影响本次登录
		if (valid && PasswordHash::NeedsRehash(origin_pwd)) {
			const auto upgraded = PasswordHash::Hash(pwd);
			if (!upgraded.empty()) {
				try {
					con->_session->sql("UPDATE user SET pwd = ? WHERE uid = ?")
						.bind(upgraded).bind(userInfo.uid).execute();
				}
				catch (mysqlx::Error& e) {
					std::cerr << "password rehash update failed: " << e.what() << std::endl;
				}
			}
		}
		return valid;
	}
	catch (mysqlx::Error& e) {
		//查询本身失败（连接断开、SQL 错误）属于服务端故障
		db_error = true;
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}


MySqlPool::MySqlPool(const std::string& host, unsigned port, const std::string& user, const std::string& pass,
	const std::string& schema, int poolSize)
		: host_(host), port_(port), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false)
{
	//逐条建立连接, 单条失败不影响其余连接; 数量缺口由检测线程首轮补足
	for (int i = 0; i < poolSize_; ++i) {
		try {
			auto settings = MakeSessionSettings(host_, port_, user_, pass_, schema_);
			auto session = std::make_unique<mysqlx::Session>(settings);
			auto currentTime = std::chrono::system_clock::now().time_since_epoch();
			long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
			pool_.push(std::make_unique<SqlConnection>(std::move(session), timestamp));
			++connection_count_;
		}
		catch (mysqlx::Error& e) {
			std::cout << "mysql connection init failed: " << e.what() << std::endl;
		}
	}
	//启动检测线程, 定期检查连接的状态并保持连接的活跃性(心跳机制)
	_check_thread = std::thread([this]() {
		while (!b_stop_) {
			//锁只覆盖等待本身, 检查与重连全部在锁外执行
			{
				std::unique_lock<std::mutex> lock(mutex_);
				//两个唤醒来源: 停机, 或业务线程丢弃连接后请求补池; 谓词在锁内求值, 不会漏掉通知
				check_cond_.wait_for(lock, std::chrono::seconds(60), [this] {
					return b_stop_.load() || check_requested_;
				});
				if (b_stop_) break;
				check_requested_ = false; //在锁内清除, 与置位方互斥, 本轮未处理的缺口仍会在本轮补足
			}
			checkConnection();
		}
	});
}

void MySqlPool::checkConnection() {
	size_t poolsize;
	{
	    std::lock_guard<std::mutex> guard(mutex_);
	    poolsize = pool_.size(); //获取当前连接池中连接的数量, 以便在遍历连接池时不受其他线程影响
	}

    //获取当前时间戳
	auto currentTime = std::chrono::system_clock::now().time_since_epoch();
	long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();

	for (size_t i = 0; i < poolsize; i++) {
		//从连接池队列中取出一个连接对象进行检查
		std::unique_ptr<SqlConnection> con;
		{
		    std::lock_guard<std::mutex> guard(mutex_);
		    //如果连接池为空, 则直接退出循环, 避免访问空队列
		    if (pool_.empty())
		        break;
		    con = std::move(pool_.front());
		    pool_.pop();
		}

		bool healthy = true;
		try {
		    //如果连接对象的最后操作时间距当前时间超过5秒, 则执行简单查询以保持连接活跃
			if (timestamp - con->_last_oper_time >= 5) {
				con->_session->sql("SELECT 1").execute(); //执行简单查询以保持连接活跃
				con->_last_oper_time = timestamp; //更新最后操作时间戳, 以便下次检查时判断是否需要保持连接活跃
			}
		}
		catch (mysqlx::Error& e) {
			std::cout << "Error keeping connection alive: " << e.what() << std::endl;
			healthy = false;
		}
		std::lock_guard<std::mutex> guard(mutex_);
		if (healthy && !b_stop_) {
			pool_.push(std::move(con));
			cond_.notify_one();
		} else {
			--connection_count_;
		}
	}
	while (!b_stop_) {
		{
			std::lock_guard<std::mutex> guard(mutex_);
			//补足到池容量: 覆盖检查失败、业务线程丢弃、启动时建连失败三种缺口
			if (connection_count_ >= static_cast<size_t>(poolSize_)) break;
		}
		if (!reconnect(timestamp)) break; //数据库不可达时不在锁外空转, 留到下一个周期再试
	}
}

bool MySqlPool::reconnect(long long timestamp) {
	try {
	    //构建X DevAPI连接设置
		auto settings = MakeSessionSettings(host_, port_, user_, pass_, schema_);
		auto session = std::make_unique<mysqlx::Session>(settings);
		auto con = std::make_unique<SqlConnection>(std::move(session), timestamp);
		{
		    std::lock_guard<std::mutex> lock(mutex_);
		    if (b_stop_)
		        return false;
		    pool_.push(std::move(con)); //将新连接对象加入连接池队列, 使其可供其他线程使用
			++connection_count_; //新增连接是池容量增长的唯一来源, 必须与队列同步记账
		}
		cond_.notify_one(); //通知一个等待的线程, 使其从连接池中获取一个可用的连接对象
	    return true;
	} catch (mysqlx::Error& e){
	    std::cerr << "MySQL reconnect failed: " << e.what() << std::endl;
	    return false;
	}
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection()
{
	std::unique_lock<std::mutex> lock(mutex_); //加锁保护连接池的状态
	//有界等待：数据库不可达或连接耗尽时快速失败，避免 HTTP IO 线程被永久挂住
	const bool ready = cond_.wait_for(lock, std::chrono::milliseconds(MYSQL_POOL_WAIT_MS), [this] {
		return b_stop_.load() || !pool_.empty();
	});
	if (b_stop_)
		return nullptr;
	if (!ready) {
		//唤醒检测线程尽快补池，DB 恢复后不必再等完整的 60 秒巡检周期
		check_requested_ = true;
		lock.unlock();
		check_cond_.notify_one();
		std::cerr << "[mysql] pool wait timeout, no connection available" << std::endl;
		return nullptr;
	}

	//从连接池中获取一个可用的连接对象
	std::unique_ptr<SqlConnection> con(std::move(pool_.front()));
	pool_.pop(); //将连接对象从连接池队列中移除
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
	if (!con) return;
	std::unique_lock<std::mutex> lock(mutex_); //加锁保护连接池的状态
	//如果连接池被标记为停止, 则直接返回, 不将连接对象归还给连接池
	if (b_stop_)
		return;
	if (!con->_healthy || !con->_session) {
		//不可复用的连接不回到队列: 先按"空闲+借出"的总数销账, 再请求检测线程补建
		--connection_count_;
		check_requested_ = true;
		lock.unlock(); //关闭会话可能涉及网络收尾, 放到锁外执行
		check_cond_.notify_one();
		return;
	}

	//归还时刷新活跃时间, 检测线程据此跳过刚用过的连接, 只对真正空闲的连接做心跳
	auto now = std::chrono::system_clock::now().time_since_epoch();
	con->_last_oper_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	pool_.push(std::move(con)); //将连接对象加入连接池队列, 使其可供其他线程使用
	cond_.notify_one(); //通知一个等待的线程, 使其从连接池中获取一个可用的连接对象
}

void MySqlPool::Close()
{
	{
	    std::lock_guard<std::mutex> lock(mutex_);
	    b_stop_ = true; //标记连接池为停止
	}
	cond_.notify_all(); //唤醒等待连接的业务线程
	check_cond_.notify_all(); //唤醒周期检测线程
    //等待检测线程退出, 确保在析构函数中不会有线程访问已释放的资源
	if (_check_thread.joinable())
	    _check_thread.join();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		//检测线程已退出, 队列中的连接不会再被取用, 直接随容器析构关闭
		while (!pool_.empty())
			pool_.pop();
		connection_count_ = 0; //停止后容量计数不再有意义, 清零避免被误读
	}
}

MySqlPool::~MySqlPool()
{
	Close(); //幂等: 调用方已关闭时这里只做一次空转, 保证检测线程一定被回收
}
