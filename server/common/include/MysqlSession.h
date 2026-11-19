#pragma once
// MySQL 会话层：Boost.MySQL（经典协议 3306）之上的同步门面
//
// 每次查询都带 deadline：到点用 Asio 的操作级取消中断当前操作，而不是判死连接。
// 被取消的连接协议状态未知，标记为污染，归还时由池丢弃并补建新的；其余连接不受影响，
// 因此数据库卡住时最多损失一个 deadline 的等待。
//
// 接口形状：sql / bind / execute / fetchOne / get<T> / isNull /
// startTransaction / commit / rollback。

#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/mysql.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

//所有数据库错误统一抛这个，DAO 只需捕获这一种
class MysqlError : public std::runtime_error {
public:
	explicit MysqlError(const std::string& message) : std::runtime_error(message) {}
};

//绑定参数：现有 SQL 只用到整型与字符串
class MysqlParam {
public:
	//非 explicit：bind() 直接收 std::string / int 等，DAO 里不用写 MysqlParam{}
	template <typename T, std::enable_if_t<std::is_integral_v<std::decay_t<T>>, int> = 0>
	MysqlParam(T value) : _int(static_cast<std::int64_t>(value)) {}
	MysqlParam(const std::string& value) : _text(value) {}
	MysqlParam(std::string_view value) : _text(std::string(value)) {}
	MysqlParam(const char* value) : _text(value ? value : "") {}

	//field 里的字符串是视图，指向 _text；调用方保证在执行期间持有本对象
	boost::mysql::field ToField() const {
		if (_text)
			return boost::mysql::field(std::string_view(*_text));
		return boost::mysql::field(_int);
	}

private:
	std::int64_t _int = 0;
	std::optional<std::string> _text;
};

//单元格：isNull 判断空值，get<T> 按类型取值
class MysqlValue {
public:
	MysqlValue() = default;
	explicit MysqlValue(boost::mysql::field_view value) : _value(value) {}

	bool isNull() const { return _value.is_null(); }

	template <typename T>
	T get() const;

private:
	boost::mysql::field_view _value{};
};

//一行结果：operator[] 取列，isNull 表示没有取到行。
//刻意持有 boost::mysql::row 而不是 row_view：DAO 里常见
//auto row = session.sql(...).execute().fetchOne(); 这种写法，
//结果是临时对象，视图会在表达式结束时悬空
class MysqlRow {
public:
	MysqlRow() = default;
	explicit MysqlRow(boost::mysql::row_view row) : _row(row), _null(false) {}

	bool isNull() const { return _null; }
	MysqlValue operator[](std::size_t index) const { return MysqlValue(_row.at(index)); }

private:
	boost::mysql::row _row;
	bool _null = true;
};

//结果集：fetchOne 逐行读取，另带影响行数
class MysqlResult {
public:
	explicit MysqlResult(boost::mysql::results results) : _results(std::move(results)) {}

	MysqlRow fetchOne();
	std::uint64_t getAffectedItemsCount() const { return _results.affected_rows(); }

private:
	boost::mysql::results _results;
	std::size_t _index = 0;
};

class MysqlSession;

//一条待执行语句：sql(...).bind(a).bind(b).execute()
class MysqlStatement {
public:
	MysqlStatement(MysqlSession& session, std::string text)
		: _session(session), _text(std::move(text)) {}

	MysqlStatement& bind(MysqlParam value) {
		_params.push_back(std::move(value));
		return *this;
	}

	MysqlResult execute();

private:
	MysqlSession& _session;
	std::string _text;
	std::vector<MysqlParam> _params;
};

//连接参数：地址、账号、库名与单条查询的 deadline
struct MysqlConnectOptions {
	std::string host = "127.0.0.1";
	unsigned port = 3306;
	std::string user = "root";
	std::string password;
	std::string schema;
	std::chrono::milliseconds query_timeout{ 5000 };
};

//一条到 MySQL 的会话：持有自己的 io_context，deadline 用操作级取消实现
class MysqlSession {
public:
	explicit MysqlSession(MysqlConnectOptions options);
	~MysqlSession();

	MysqlSession(const MysqlSession&) = delete;
	MysqlSession& operator=(const MysqlSession&) = delete;

	bool Connect(); //建连并设置会话级上限；失败返回 false
	//超时取消过的连接协议状态未知，不再复用：IsOpen 返回 false，池会换一条新的
	bool IsOpen() const { return _open && !_tainted; }
	bool Ping(); //连通性探测，给巡检与健康检查用

	MysqlStatement sql(std::string text) { return MysqlStatement(*this, std::move(text)); }
	void startTransaction() { Execute("START TRANSACTION", {}); }
	void commit() { Execute("COMMIT", {}); }
	void rollback() { Execute("ROLLBACK", {}); }

	MysqlResult Execute(const std::string& text, const std::vector<MysqlParam>& params);

private:
	MysqlConnectOptions _options;
	boost::asio::io_context _context;
	boost::mysql::any_connection _connection;
	//同一条 SQL 只准备一次；DAO 里的语句是固定集合，连接生命周期内复用
	std::unordered_map<std::string, boost::mysql::statement> _statements;
	bool _open = false;
	bool _tainted = false;
};

//取值统一走转换函数：库里的 id 列多为 unsigned，直接 as_int64 会抛 bad_value_access；
//NULL 返回 0 / 空串，调用方按"没有值"处理
std::int64_t MysqlFieldToInt64(const boost::mysql::field_view& value);
std::string MysqlFieldToString(const boost::mysql::field_view& value);

template <>
inline int MysqlValue::get<int>() const { return static_cast<int>(MysqlFieldToInt64(_value)); }
template <>
inline std::int64_t MysqlValue::get<std::int64_t>() const { return MysqlFieldToInt64(_value); }
template <>
inline std::string MysqlValue::get<std::string>() const { return MysqlFieldToString(_value); }
