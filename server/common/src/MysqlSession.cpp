#include "MysqlSession.h"
#include "Log.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace {

namespace asio = boost::asio;
namespace mysql = boost::mysql;

std::string Describe(const boost::system::error_code& ec, const mysql::diagnostics& diag) {
	std::string message = ec.message();
	const std::string server = diag.server_message();
	if (!server.empty())
		message += " (" + server + ")";
	return message;
}

mysql::connect_params MakeConnectParams(const MysqlConnectOptions& options) {
	mysql::connect_params params;
	params.server_address.emplace_host_and_port(options.host,
		static_cast<unsigned short>(options.port));
	params.username = options.user;
	params.password = options.password;
	params.database = options.schema;
	return params;
}

//所有网络操作共用：到点用操作级取消打断。取消后协议里可能残留未读完的
//结果包，连接不再复用（调用方据 ec 标记污染），由池换一条新连接
template <typename Start>
boost::system::error_code RunWithDeadline(asio::io_context& context,
	std::chrono::milliseconds timeout, Start start) {
	boost::system::error_code ec;
	asio::cancellation_signal signal;
	asio::steady_timer timer(context);
	timer.expires_after(timeout);
	timer.async_wait([&signal](const boost::system::error_code& e) {
		if (!e)
			signal.emit(asio::cancellation_type::terminal);
		});
	start(asio::bind_cancellation_slot(signal.slot(),
		[&ec, &timer](boost::system::error_code e) {
			ec = e;
			timer.cancel();
		}));
	context.restart();
	context.run();
	return ec;
}

//服务端 SQL 错误（唯一键冲突、锁等待超时、查询超时 3024）只影响这一条语句，
//连接本身仍然可用；其余错误（网络中断、协议错位）说明会话状态不可信，
//标记为污染，归还时由池丢弃并补建
bool IsServerSqlError(const boost::system::error_code& ec) {
	return ec.category() == boost::mysql::get_mysql_server_category();
}

//整数列可能是 signed 或 unsigned，按实际 kind 取；字符串/小数按字面量解析
std::int64_t ParseSigned(const boost::mysql::field_view& value) {
	switch (value.kind()) {
	case boost::mysql::field_kind::null:
		return 0;
	case boost::mysql::field_kind::int64:
		return value.as_int64();
	case boost::mysql::field_kind::uint64:
		return static_cast<std::int64_t>(value.as_uint64());
	case boost::mysql::field_kind::float_:
		return static_cast<std::int64_t>(value.as_float());
	case boost::mysql::field_kind::double_:
		return static_cast<std::int64_t>(value.as_double());
	case boost::mysql::field_kind::string:
		return std::strtoll(std::string(value.as_string()).c_str(), nullptr, 10);
	case boost::mysql::field_kind::blob: {
		const auto blob = value.as_blob();
		const std::string text(reinterpret_cast<const char*>(blob.data()), blob.size());
		return std::strtoll(text.c_str(), nullptr, 10);
	}
	default:
		return 0; //日期时间列不该走整型读取，按 0 处理而不是抛异常
	}
}

} // namespace

std::int64_t MysqlFieldToInt64(const boost::mysql::field_view& value) {
	return ParseSigned(value);
}

std::string MysqlFieldToString(const boost::mysql::field_view& value) {
	switch (value.kind()) {
	case boost::mysql::field_kind::null:
		return {};
	case boost::mysql::field_kind::string:
		return std::string(value.as_string());
	case boost::mysql::field_kind::blob: {
		const auto blob = value.as_blob();
		return std::string(reinterpret_cast<const char*>(blob.data()), blob.size());
	}
	case boost::mysql::field_kind::int64:
		return std::to_string(value.as_int64());
	case boost::mysql::field_kind::uint64:
		return std::to_string(value.as_uint64());
	default:
		return {};
	}
}

MysqlRow MysqlResult::fetchOne() {
	const auto rows = _results.rows();
	if (_index >= rows.size())
		return MysqlRow();
	return MysqlRow(rows.at(_index++));
}

MysqlResult MysqlStatement::execute() {
	return _session.Execute(_text, _params);
}

MysqlSession::MysqlSession(MysqlConnectOptions options)
	: _options(std::move(options)), _connection(_context) {}

MysqlSession::~MysqlSession() {
	//不发 COM_QUIT：服务端被暂停或网络黑洞时，优雅关闭会卡在写 socket 上，
	//把一条已经出错的连接变成一条卡死的线程。直接把 socket 交给析构销毁，
	//MySQL 会按正常断开回收会话。
}

bool MysqlSession::Connect() {
	if (_open)
		return true;

	mysql::connect_params params = MakeConnectParams(_options);
	mysql::diagnostics diag;
	const auto connect_timeout = std::min(_options.query_timeout, std::chrono::milliseconds(2000));
	const boost::system::error_code ec = RunWithDeadline(_context, connect_timeout,
		[&](auto handler) {
			_connection.async_connect(params, diag, std::move(handler));
		});

	if (ec) {
		LOG_INFO << "mysql connect failed: " << Describe(ec, diag)
			<< " host=" << _options.host << ":" << _options.port;
		return false;
	}

	//先标记可用：下面的 SET 也走 Execute，会检查这个标志
	_open = true;

	//会话级上限：慢查询与锁等待由 MySQL 主动中止，客户端 deadline 再兜底一层。
	//个别兼容实现（MariaDB）不认这些变量，设不上只告警，不影响连接可用
	try {
		Execute("SET SESSION max_execution_time = 4000", {});
		Execute("SET SESSION innodb_lock_wait_timeout = 3", {});
		Execute("SET SESSION lock_wait_timeout = 10", {});
	}
	catch (const MysqlError& e) {
		LOG_WARN << "mysql session limits not applied: " << e.what();
	}
	return true;
}

bool MysqlSession::Ping() {
	try {
		sql("SELECT 1").execute();
		return true;
	}
	catch (const MysqlError&) {
		return false;
	}
}

MysqlResult MysqlSession::Execute(const std::string& text, const std::vector<MysqlParam>& params) {
	if (!_open)
		throw MysqlError("mysql session is not connected");

	//无参语句直接发文本，省一次 prepare 往返
	if (params.empty()) {
		mysql::results results;
		mysql::diagnostics diag;
		const boost::system::error_code ec = RunWithDeadline(_context, _options.query_timeout,
			[&](auto handler) {
				_connection.async_execute(text, results, diag, std::move(handler));
			});
		if (ec) {
			if (!IsServerSqlError(ec))
				_tainted = true; //deadline 或传输层故障：会话状态不可信，丢弃这条连接
			throw MysqlError(Describe(ec, diag) + " | sql=" + text.substr(0, 120));
		}
		return MysqlResult(std::move(results));
	}

	//有参语句：首次准备后缓存，之后复用；绑定用迭代器范围，支持运行期参数个数
	auto it = _statements.find(text);
	if (it == _statements.end()) {
		mysql::statement statement;
		mysql::diagnostics prepare_diag;
		boost::system::error_code prepare_ec;
		asio::cancellation_signal signal;
		asio::steady_timer timer(_context);
		timer.expires_after(_options.query_timeout);
		timer.async_wait([&signal](const boost::system::error_code& e) {
			if (!e)
				signal.emit(asio::cancellation_type::terminal);
			});
		//prepare 的完成签名带 statement，用不了单参数的 RunWithDeadline
		_connection.async_prepare_statement(text, prepare_diag,
			asio::bind_cancellation_slot(signal.slot(),
				[&](boost::system::error_code e, mysql::statement st) {
					prepare_ec = e;
					if (!e)
						statement = std::move(st);
					timer.cancel();
				}));
		_context.restart();
		_context.run();
		if (prepare_ec) {
			if (!IsServerSqlError(prepare_ec))
				_tainted = true;
			throw MysqlError(Describe(prepare_ec, prepare_diag) + " | prepare=" + text.substr(0, 120));
		}
		it = _statements.emplace(text, std::move(statement)).first;
	}

	std::vector<mysql::field> fields;
	fields.reserve(params.size());
	for (const auto& param : params)
		fields.push_back(param.ToField());

	mysql::results results;
	mysql::diagnostics diag;
	auto bound = it->second.bind(fields.begin(), fields.end());
	const boost::system::error_code ec = RunWithDeadline(_context, _options.query_timeout,
		[&](auto handler) {
			_connection.async_execute(bound, results, diag, std::move(handler));
		});
	if (ec) {
		if (!IsServerSqlError(ec))
			_tainted = true;
		else if (ec.value() == 1243)
			_statements.erase(it); //服务端已丢弃该句柄（改表/刷新），下次重新准备
		throw MysqlError(Describe(ec, diag) + " | sql=" + text.substr(0, 120));
	}
	return MysqlResult(std::move(results));
}
