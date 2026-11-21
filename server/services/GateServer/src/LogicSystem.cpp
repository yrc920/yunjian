#include "LogicSystem.h"
#include "JsonUtil.h"
#include "HttpConnection.h"
#include "VerifyGrpcClient.h"
#include "RedisMgr.h"
#include "MysqlDao.h"
#include "StatusGrpcClient.h"
#include "PasswordPool.h"
#include <chrono>
#include <ctime>
#include <cctype>
#include <cstdlib>
#include <functional>
#include "Log.h"

namespace {

//日志里不留完整邮箱：保留首字符与域名，能定位账号又不会把通讯录写进日志
std::string MaskEmail(const std::string& email)
{
	const auto at = email.find('@');
	if (at == std::string::npos || at == 0)
		return "***";
	return email.substr(0, 1) + "***" + email.substr(at);
}

//进程启动时刻，健康检查用来回报运行时长
std::chrono::steady_clock::time_point ProcessStartTime()
{
	static const auto start = std::chrono::steady_clock::now();
	return start;
}

//取客户端地址做 IP 维度限流；取不到时返回空串，调用方跳过该维度
std::string ClientIp(const std::shared_ptr<HttpConnection>& connection)
{
	boost::system::error_code ec;
	auto endpoint = connection->GetSocket().remote_endpoint(ec);
	if (ec)
		return {};
	return endpoint.address().to_string();
}

//邮箱在数据库里按不区分大小写比较，在 Redis 里却区分：
//入口统一 trim + 小写，验证码、注册、登录、重置才能落到同一个键上
std::string NormalizeEmail(const std::string& input)
{
	std::string email = input;
	const auto begin = email.find_first_not_of(" \t\r\n");
	const auto end = email.find_last_not_of(" \t\r\n");
	email = begin == std::string::npos ? "" : email.substr(begin, end - begin + 1);
	for (char& ch : email)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return email;
}

bool IsValidEmail(const std::string& email)
{
	if (email.size() < 5 || email.size() > 254)
		return false;
	const auto at = email.find('@');
	if (at == std::string::npos || at == 0 || at + 1 >= email.size())
		return false;
	if (email.find('@', at + 1) != std::string::npos)
		return false;
	if (at > 64)
		return false;
	const std::string domain = email.substr(at + 1);
	if (domain.front() == '.' || domain.back() == '.' || domain.find('.') == std::string::npos)
		return false;
	for (unsigned char ch : email) {
		if (ch <= 0x20 || ch == 0x7f)
			return false;
	}
	return true;
}

//用户名按字符数限制 2~20；拒绝空白、控制字符与路径/协议分隔符，
//中文与 emoji 按 UTF-8 首字节计数，不把一个汉字算成三字节
bool IsValidUserName(const std::string& name)
{
	std::size_t chars = 0;
	for (unsigned char ch : name) {
		if ((ch & 0xC0) == 0x80)
			continue;
		++chars;
		if (ch <= 0x20 || ch == 0x7f)
			return false;
		if (ch < 0x80 && std::string("@/\\:*?\"<>|").find(static_cast<char>(ch)) != std::string::npos)
			return false;
	}
	return chars >= 2 && chars <= 20;
}

//客户端发送的是 xor 变形后的口令，长度与原文一致。
//这里只校验长度与非空：具体字符集由客户端约束，服务端不做可逆解析
bool IsValidPasswordPayload(const std::string& pwd)
{
	return pwd.size() >= 6 && pwd.size() <= 15
		&& pwd.find('\0') == std::string::npos;
}

//把"哈希 + 落库"放到口令线程池执行：Argon2id 单次 64 MiB，
//在单线程 io_context 上做会把所有 HTTP 请求一起卡住。
//work 在池线程算完得到回包，再回投到连接所属的 IO 线程写响应；
//队列排满时立刻回可重试错误，不让请求等到超时
void PostPasswordWork(const std::shared_ptr<HttpConnection>& connection,
	std::function<Json::Value()> work)
{
	connection->DeferResponse();
	const bool queued = PasswordPool::Instance().Post(
		[connection, work = std::move(work)]() {
			Json::Value response;
			try {
				response = work();
			}
			catch (const std::exception& e) {
				//任务异常也要回包，否则连接只能等到 15 秒截止时间才被关闭
				LOG_ERROR << "password work failed: " << e.what();
				response["error"] = ErrorCodes::RPCFailed;
			}
			boost::asio::post(connection->GetSocket().get_executor(),
				[connection, response]() {
					connection->SendDeferred(JsonToCompact(response));
				});
		});
	if (queued)
		return;

	Json::Value busy;
	busy["error"] = ErrorCodes::ServiceUnavailable;
	connection->SendDeferred(JsonToCompact(busy));
}

//所有同步入口共用的写回包：统一 content-type 与紧凑 JSON
void WriteJson(const std::shared_ptr<HttpConnection>& connection, const Json::Value& response)
{
	connection->SetJsonResponse(JsonToCompact(response));
}

void Fail(const std::shared_ptr<HttpConnection>& connection, int error)
{
	Json::Value response;
	response["error"] = error;
	WriteJson(connection, response);
}

//解析请求体并检查必填字段的类型：所有字段在协议里都是字符串。
//失败时由调用方回 Error_Json，不在这里写响应，避免异步分支重复发送
bool ReadJson(const std::shared_ptr<HttpConnection>& connection, Json::Value& root,
	std::initializer_list<const char*> required)
{
	if (!JsonFromString(connection->RequestBody(), root) || !root.isObject())
		return false;
	for (const char* key : required) {
		if (!root[key].isString())
			return false;
	}
	return true;
}

//登录限流键统一小写：MySQL 的邮箱比较不区分大小写，Redis 键区分，
//不归一化的话换个大小写就能绕过同一账号的失败计数
std::string ToLowerAscii(std::string text)
{
	for (char& ch : text)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	return text;
}

long long ReadCounter(const std::string& key)
{
	std::string value;
	if (!RedisMgr::GetInstance()->Get(key, value))
		return 0;
	return std::strtoll(value.c_str(), nullptr, 10);
}

} // namespace

LogicSystem::LogicSystem() {
	//健康检查：部署脚本与容器探针用，任一依赖不可用就回 503，
	//响应体带上细分原因，排障时不用再登机器看日志
	RegGet("/healthz", [](std::shared_ptr<HttpConnection> connection) {
		const bool redis_ok = RedisMgr::GetInstance()->Ping();
		const bool mysql_ok = MysqlDao::GetInstance()->Ping();
		const bool healthy = redis_ok && mysql_ok;

		Json::Value root;
		root["status"] = healthy ? "ok" : "degraded";
		root["redis"] = redis_ok;
		root["mysql"] = mysql_ok;
		root["uptime_seconds"] = static_cast<Json::Int64>(
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - ProcessStartTime()).count());

		connection->_response.set(http::field::content_type, "application/json");
		connection->SetResult(healthy ? http::status::ok : http::status::service_unavailable);
		beast::ostream(connection->_response.body()) << JsonToCompact(root);
		return true;
	});

	RegPost("/get_verifycode", [](std::shared_ptr<HttpConnection> connection) {
		Json::Value root;
		if (!ReadJson(connection, root, { "email" })) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		const std::string email = NormalizeEmail(root["email"].asString());
		if (!IsValidEmail(email)) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		//发送频控：先卡 60 秒冷却，再卡邮箱与 IP 的小时配额。
		//Redis 不可用时按 RPC 失败返回，不能把限流器变成放行器
		const long long hour_bucket = std::time(nullptr) / CODE_RATE_WINDOW_SECONDS;
		const int cooldown = RedisMgr::GetInstance()->SetNxEx(
			COOLDOWNPREFIX + email, "1", CODE_COOLDOWN_SECONDS);
		if (cooldown <= 0) {
			Json::Value response;
			response["error"] = cooldown < 0 ? ErrorCodes::RPCFailed : ErrorCodes::VerifyTooFrequent;
			response["email"] = email;
			WriteJson(connection, response);
			return true;
		}

		int rate_error = 0;
		const long long email_count = RedisMgr::GetInstance()->IncrWithTtl(
			RATEEMAILPREFIX + email + "_" + std::to_string(hour_bucket),
			CODE_RATE_WINDOW_SECONDS);
		if (email_count < 0)
			rate_error = ErrorCodes::RPCFailed;
		else if (email_count > CODE_EMAIL_HOURLY_LIMIT)
			rate_error = ErrorCodes::VerifyTooFrequent;

		const std::string ip = ClientIp(connection);
		if (rate_error == 0 && !ip.empty()) {
			const long long ip_count = RedisMgr::GetInstance()->IncrWithTtl(
				RATEIPPREFIX + ip + "_" + std::to_string(hour_bucket),
				CODE_RATE_WINDOW_SECONDS);
			if (ip_count < 0)
				rate_error = ErrorCodes::RPCFailed;
			else if (ip_count > CODE_IP_HOURLY_LIMIT)
				rate_error = ErrorCodes::VerifyTooFrequent;
		}
		if (rate_error != 0) {
			Json::Value response;
			response["error"] = rate_error;
			response["email"] = email;
			WriteJson(connection, response);
			return true;
		}

		//验证服务返回的就是统一错误码：0 成功 / 1002 依赖失败 / 1023 邮件投递失败
		const GetVerifyRsp rsp = VerifyGrpcClient::GetInstance()->GetVerifyCode(email);
		LOG_DEBUG << "verify code requested for " << MaskEmail(email);
		Json::Value response;
		response["error"] = rsp.error();
		response["email"] = email;
		WriteJson(connection, response);
		return true;
	});

	RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection) {
		Json::Value root;
		if (!ReadJson(connection, root,
			{ "email", "user", "passwd", "confirm", "verifycode" })) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		const std::string email = NormalizeEmail(root["email"].asString());
		const std::string name = root["user"].asString();
		const std::string pwd = root["passwd"].asString();
		if (!IsValidEmail(email) || !IsValidUserName(name) || !IsValidPasswordPayload(pwd)) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}
		if (pwd != root["confirm"].asString()) {
			Fail(connection, ErrorCodes::PasswdErr);
			return true;
		}

		std::string verify_code;
		if (!RedisMgr::GetInstance()->Get(CODEPREFIX + email, verify_code)) {
			Fail(connection, ErrorCodes::VerifyExpired);
			return true;
		}
		if (verify_code != root["verifycode"].asString()) {
			Fail(connection, ErrorCodes::VerifyCodeErr);
			return true;
		}

		//注册要写库并生成哈希，同样交给口令线程池
		PostPasswordWork(connection, [name, email, pwd]() {
			Json::Value result;
			const int uid = MysqlDao::GetInstance()->RegUser(name, email, pwd);
			if (uid == 0 || uid == -1) {
				LOG_WARN << " account already exists: " << MaskEmail(email);
				result["error"] = ErrorCodes::UserExist;
				return result;
			}

			//验证码一次性使用：注册成功后立即删除，避免同一码再用于改密
			RedisMgr::GetInstance()->Del(CODEPREFIX + email);

			spdlog::info("register ok uid={} account={} name={}", uid, MaskEmail(email), name);
			result["error"] = 0;
			result["uid"] = uid;
			result["email"] = email;
			result["user"] = name;
			return result;
		});
		return true;
	});

	RegPost("/reset_pwd", [](std::shared_ptr<HttpConnection> connection) {
		Json::Value root;
		if (!ReadJson(connection, root, { "email", "user", "passwd", "verifycode" })) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		const std::string email = NormalizeEmail(root["email"].asString());
		const std::string name = root["user"].asString();
		const std::string pwd = root["passwd"].asString();
		if (!IsValidEmail(email) || name.empty() || !IsValidPasswordPayload(pwd)) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		std::string verify_code;
		if (!RedisMgr::GetInstance()->Get(CODEPREFIX + email, verify_code)) {
			Fail(connection, ErrorCodes::VerifyExpired);
			return true;
		}
		if (verify_code != root["verifycode"].asString()) {
			Fail(connection, ErrorCodes::VerifyCodeErr);
			return true;
		}

		PostPasswordWork(connection, [name, email, pwd]() {
			Json::Value result;
			bool db_error = false;
			const bool matched = MysqlDao::GetInstance()->CheckEmail(name, email, db_error);
			if (db_error) {
				result["error"] = ErrorCodes::RPCFailed;
				return result;
			}
			if (!matched) {
				result["error"] = ErrorCodes::EmailNotMatch;
				return result;
			}
			if (!MysqlDao::GetInstance()->UpdatePwd(name, pwd)) {
				result["error"] = ErrorCodes::PasswdUpFailed;
				return result;
			}

			RedisMgr::GetInstance()->Del(CODEPREFIX + email);
			//改密后旧令牌立即失效：否则自动登录与已泄露的令牌可以绕过这次重置
			const int uid = MysqlDao::GetInstance()->GetUidByName(name);
			if (uid > 0)
				RedisMgr::GetInstance()->Del(UserTokenKey(uid));
			spdlog::info("password reset ok account={} name={} token revoked uid={}",
				MaskEmail(email), name, uid);
			result["error"] = 0;
			result["email"] = email;
			result["user"] = name;
			return result;
		});
		return true;
	});

	RegPost("/user_login", [](std::shared_ptr<HttpConnection> connection) {
		Json::Value root;
		if (!ReadJson(connection, root, { "email", "passwd" })) {
			Fail(connection, ErrorCodes::Error_Json);
			return true;
		}

		const std::string email = NormalizeEmail(root["email"].asString());
		const std::string pwd = root["passwd"].asString();
		if (!IsValidEmail(email) || !IsValidPasswordPayload(pwd)) {
			Fail(connection, ErrorCodes::PasswdInvalid);
			return true;
		}

		//登录失败限流：先查失败计数，超限时直接返回，不再做 256MB 的密码校验。
		//只统计失败、成功清零，正常用户偶发输错不会被长期锁住
		const std::string account = ToLowerAscii(email);
		const long long bucket = std::time(nullptr) / LOGIN_FAIL_WINDOW_SECONDS;
		const std::string account_key = LOGIN_FAIL_PREFIX + account + "_" + std::to_string(bucket);
		const std::string ip = ClientIp(connection);
		const std::string ip_key = ip.empty() ? std::string()
			: LOGIN_FAIL_IP_PREFIX + ip + "_" + std::to_string(bucket);
		const long long account_fails = ReadCounter(account_key);
		const long long ip_fails = ip_key.empty() ? 0 : ReadCounter(ip_key);
		if (account_fails >= LOGIN_ACCOUNT_FAIL_LIMIT || ip_fails >= LOGIN_IP_FAIL_LIMIT) {
			Fail(connection, ErrorCodes::TooManyRequests);
			return true;
		}

		//剩下的活都绕不开密码校验：交给口令线程池，IO 线程继续接别的连接
		PostPasswordWork(connection, [email, pwd, account_key, ip_key]() {
			Json::Value result;
			UserInfo user_info;
			bool db_error = false;
			const bool pwd_valid = MysqlDao::GetInstance()->CheckPwd(email, pwd, user_info, db_error);
			if (!pwd_valid) {
				//数据库不可用是服务端故障，不能当成密码错误，也不能累加失败次数
				if (db_error) {
					result["error"] = ErrorCodes::RPCFailed;
					return result;
				}
				RedisMgr::GetInstance()->IncrWithTtl(account_key, LOGIN_FAIL_WINDOW_SECONDS);
				if (!ip_key.empty())
					RedisMgr::GetInstance()->IncrWithTtl(ip_key, LOGIN_FAIL_WINDOW_SECONDS);
				spdlog::warn("login rejected account={} reason=bad_password", MaskEmail(email));
				result["error"] = ErrorCodes::PasswdInvalid;
				return result;
			}

			RedisMgr::GetInstance()->Del(account_key);
			if (!ip_key.empty())
				RedisMgr::GetInstance()->Del(ip_key);

			const auto reply = StatusGrpcClient::GetInstance()->GetChatServer(user_info.uid);
			if (reply.error()) {
				LOG_ERROR << "grpc get chat server failed, error is " << reply.error();
				result["error"] = ErrorCodes::RPCFailed;
				return result;
			}

			spdlog::info("login ok uid={} account={} chat={}:{}",
				user_info.uid, MaskEmail(email), reply.host(), reply.port());
			result["error"] = 0;
			result["email"] = email;
			result["uid"] = user_info.uid;
			result["token"] = reply.token();
			result["host"] = reply.host();
			result["port"] = reply.port();
			result["res_host"] = reply.res_host();
			result["res_port"] = reply.res_port();
			return result;
		});
		return true;
	});
}

void LogicSystem::RegGet(std::string url, HttpHandler handler) {
	_get_handlers.insert(make_pair(url, handler));
}

void LogicSystem::RegPost(std::string url, HttpHandler handler) {
	_post_handlers.insert(make_pair(url, handler));
}

bool LogicSystem::HandleGet(std::string path, std::shared_ptr<HttpConnection> con) {
	//如果没有找到对应URL的处理函数, 返回false, 让网络层返回404错误
	if (_get_handlers.find(path) == _get_handlers.end()) {
		return false;
	}
	_get_handlers[path](con);
	return true;
}

bool LogicSystem::HandlePost(std::string path, std::shared_ptr<HttpConnection> con) {
	if (_post_handlers.find(path) == _post_handlers.end()) {
		return false;
	}
	_post_handlers[path](con);
	return true;
}
