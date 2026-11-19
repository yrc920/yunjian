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
			Json::Value response = work();
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
	//注册一个GET请求的URL和对应的处理函数
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

	//注册发送验证码的POST请求的URL和对应的处理函数
	RegPost("/get_verifycode", [](std::shared_ptr<HttpConnection> connection) {
		//将请求体中的数据转换为字符串
		auto body_str = beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "application/json"); //设置响应内容类型为JSON

		Json::Value root; //用于构建响应的JSON数据
		Json::Reader reader; //用于解析请求体中的JSON数据
		Json::Value src_root; //用于存储解析后的JSON数据
		//解析请求体中的JSON数据, 将解析结果存储到src_root中
		bool parse_success = reader.parse(body_str, src_root);
		//请求体必须是对象，email必须是字符串；先检查类型再取值，避免数组或对象触发转换异常
		if (!parse_success || !src_root.isObject() || !src_root["email"].isString()) {
			LOG_WARN << "Failed to parse JSON data!";
			root["error"] = ErrorCodes::Error_Json; //设置错误码为JSON解析错误
			//将JSON数据转换为字符串, 并写入响应内容中
			std::string jsonstr = JsonToCompact(root);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

		//如果解析成功, 则从src_root中提取email字段的值
		auto email = src_root["email"].asString();

		//发送频控：先卡 60 秒冷却，再卡邮箱与 IP 的小时配额。
		//Redis 不可用时按 RPC 失败返回，不能把限流器变成放行器
		const long long hour_bucket = std::time(nullptr) / CODE_RATE_WINDOW_SECONDS;
		const int cooldown = RedisMgr::GetInstance()->SetNxEx(
			COOLDOWNPREFIX + email, "1", CODE_COOLDOWN_SECONDS);
		if (cooldown <= 0) {
			root["error"] = cooldown < 0 ? ErrorCodes::RPCFailed : ErrorCodes::VerifyTooFrequent;
			root["email"] = email;
			beast::ostream(connection->_response.body()) << JsonToCompact(root);
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
			root["error"] = rate_error;
			root["email"] = email;
			beast::ostream(connection->_response.body()) << JsonToCompact(root);
			return true;
		}

		//给验证服务发送RPC请求, 获取验证码, 将RPC调用的结果存储在rsp中
		GetVerifyRsp rsp = VerifyGrpcClient::GetInstance()->GetVerifyCode(email);
		LOG_DEBUG << "email is " << email;
		root["error"] = rsp.error(); //将RPC调用结果中的error字段设置到响应的JSON数据中
		root["email"] = src_root["email"]; //将请求中的email字段原样返回到响应中
		//将JSON数据转换为字符串, 并写入响应内容中
		std::string jsonstr = JsonToCompact(root);
		beast::ostream(connection->_response.body()) << jsonstr;
		return true;
	});

	//注册用户注册的POST请求的URL和对应的处理函数
    RegPost("/user_register", [](std::shared_ptr<HttpConnection> connection) {
		//将请求体中的数据转换为字符串
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "application/json"); //设置响应内容类型为JSON

		Json::Value root; //用于构建响应的JSON数据
		Json::Reader reader; //用于解析请求体中的JSON数据
		Json::Value src_root; //用于存储解析后的JSON数据
		//解析请求体中的JSON数据, 将解析结果存储到src_root中
        bool parse_success = reader.parse(body_str, src_root);
		//注册字段不仅要存在，还必须都是字符串，后面才可以安全地读取邮箱、账号、密码和验证码
        if (!parse_success || !src_root.isObject() ||
			!src_root["email"].isString() || !src_root["user"].isString() ||
			!src_root["passwd"].isString() || !src_root["confirm"].isString() ||
			!src_root["verifycode"].isString()) {
            LOG_WARN << "Failed to parse JSON data!";
			root["error"] = ErrorCodes::Error_Json; //设置错误码为JSON解析错误
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

		//如果解析成功, 则从src_root中提取email、user、passwd、confirm和verifycode字段的值
		auto email = src_root["email"].asString();
		auto name = src_root["user"].asString();
		auto pwd = src_root["passwd"].asString();
		auto confirm = src_root["confirm"].asString();

		//检查密码和确认密码是否一致, 如果不一致则返回错误信息
		if (pwd != confirm) {
			LOG_WARN << " confirm password error";
			root["error"] = ErrorCodes::PasswdErr;
			//将JSON数据转换为字符串, 并写入响应内容中
			std::string jsonstr = JsonToCompact(root);
			beast::ostream(connection->_response.body()) << jsonstr;
			return true;
		}

        //先查找redis中email对应的验证码是否合理
        std::string  verify_code;
        bool b_get_verify = RedisMgr::GetInstance()->
            Get(CODEPREFIX + email, verify_code);
		//如果没有找到验证码或者验证码过期了, 则返回错误信息
        if (!b_get_verify) {
            LOG_WARN << " get verify code expired";
			root["error"] = ErrorCodes::VerifyExpired; //设置错误码为验证码过期
            //将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }
		//如果找到了验证码, 则将请求中的验证码和redis中获取到的验证码进行比较, 如果不一致则返回错误信息
        if (verify_code != src_root["verifycode"].asString()) {
            LOG_WARN << " verify code error";
			root["error"] = ErrorCodes::VerifyCodeErr; //设置错误码为验证码错误
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

		//注册要写库并生成哈希，同样交给口令线程池
		PostPasswordWork(connection, [name, email, pwd]() {
			Json::Value result;
			const int uid = MysqlDao::GetInstance()->RegUser(name, email, pwd);
			if (uid == 0 || uid == -1) {
				LOG_WARN << " user or email exist";
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

	//注册用户重置密码的POST请求的URL和对应的处理函数
    RegPost("/reset_pwd", [](std::shared_ptr<HttpConnection> connection) {
        //将请求体中的数据转化为字符串
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "application/json"); //设置响应内容类型为JSON

		Json::Value root; //用于构建响应的JSON数据
		Json::Reader reader; //用于解析请求体中的JSON数据
		Json::Value src_root; //用于存储解析后的JSON数据
		//解析请求体中的JSON数据, 将解析结果存储到src_root中
        bool parse_success = reader.parse(body_str, src_root);
		//改密请求必须同时提供字符串类型的邮箱、用户名、新密码和验证码，缺字段或类型不符都返回JSON错误
        if (!parse_success || !src_root.isObject() || !src_root["email"].isString() ||
            !src_root["user"].isString() || !src_root["passwd"].isString() || !src_root["verifycode"].isString()) {
            LOG_WARN << "Failed to parse JSON data!";
			root["error"] = ErrorCodes::Error_Json; //设置错误码为JSON解析错误
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

		//如果解析成功, 则从src_root中提取email、user、passwd和verifycode字段的值
        auto email = src_root["email"].asString();
        auto name = src_root["user"].asString();
        auto pwd = src_root["passwd"].asString();

        //先查找redis中email对应的验证码是否合理
        std::string verify_code;
        bool b_get_verify = RedisMgr::GetInstance()->
			Get(CODEPREFIX + src_root["email"].asString(), verify_code);
		//如果没有找到验证码或者验证码过期了, 则返回错误信息
        if (!b_get_verify) {
            LOG_WARN << " get verify code expired";
			root["error"] = ErrorCodes::VerifyExpired; //设置错误码为验证码过期
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }
		//如果找到了验证码, 则将请求中的验证码和redis中获取到的验证码进行比较, 如果不一致则返回错误信息
        if (verify_code != src_root["verifycode"].asString()) {
            LOG_WARN << " verify code error";
			root["error"] = ErrorCodes::VerifyCodeErr; //设置错误码为验证码错误
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

        //改密要写新哈希，和注册一样交给口令线程池
		PostPasswordWork(connection, [name, email, pwd]() {
			Json::Value result;
			if (!MysqlDao::GetInstance()->CheckEmail(name, email)) {
				LOG_WARN << " user email not match";
				result["error"] = ErrorCodes::EmailNotMatch;
				return result;
			}

			if (!MysqlDao::GetInstance()->UpdatePwd(name, pwd)) {
				LOG_ERROR << " update pwd failed";
				result["error"] = ErrorCodes::PasswdUpFailed;
				return result;
			}

			//验证码一次性使用：改密成功后立即删除，避免同一码再次改密
			RedisMgr::GetInstance()->Del(CODEPREFIX + email);

			spdlog::info("password reset ok account={} name={}", MaskEmail(email), name);
			result["error"] = 0;
			result["email"] = email;
			result["user"] = name;
			return result;
		});
		return true;
        });

	//注册用户登录的POST请求的URL和对应的处理函数
    RegPost("/user_login", [](std::shared_ptr<HttpConnection> connection) {
		//将请求体中的数据转换为字符串
        auto body_str = boost::beast::buffers_to_string(connection->_request.body().data());
		connection->_response.set(http::field::content_type, "application/json"); //设置响应内容类型为JSON

		Json::Value root; //用于构建响应的JSON数据
		Json::Reader reader; //用于解析请求体中的JSON数据
		Json::Value src_root; //用于存储解析后的JSON数据
		//解析请求体中的JSON数据, 将解析结果存储到src_root中
        bool parse_success = reader.parse(body_str, src_root);
		//登录入口只接受对象形式的请求，并检查邮箱和密码类型，避免错误字段进入数据库验证流程
        if (!parse_success || !src_root.isObject() ||
            !src_root["email"].isString() || !src_root["passwd"].isString()) {
            LOG_WARN << "Failed to parse JSON data!";
			root["error"] = ErrorCodes::Error_Json; //设置错误码为JSON解析错误
			//将JSON数据转换为字符串, 并写入响应内容中
            std::string jsonstr = JsonToCompact(root);
            beast::ostream(connection->_response.body()) << jsonstr;
            return true;
        }

		//如果解析成功, 则从src_root中提取email和passwd字段的值
        auto email = src_root["email"].asString();
        auto pwd = src_root["passwd"].asString();

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
			root["error"] = ErrorCodes::TooManyRequests;
			beast::ostream(connection->_response.body()) << JsonToCompact(root);
			return true;
		}

        //剩下的活都绕不开密码校验：交给口令线程池，IO 线程继续接别的连接
		PostPasswordWork(connection,
			[email, pwd, account_key, ip_key]() {
				Json::Value result;
				UserInfo user_info;
				bool db_error = false;
				const bool pwd_valid = MysqlDao::GetInstance()->CheckPwd(
					email, pwd, user_info, db_error);
				if (!pwd_valid) {
					//数据库不可用是服务端故障，不能当成密码错误，也不能累加失败次数
					if (db_error) {
						result["error"] = ErrorCodes::RPCFailed;
						return result;
					}
					//失败计数带 60 秒 TTL，窗口结束后自动归零
					RedisMgr::GetInstance()->IncrWithTtl(account_key, LOGIN_FAIL_WINDOW_SECONDS);
					if (!ip_key.empty())
						RedisMgr::GetInstance()->IncrWithTtl(ip_key, LOGIN_FAIL_WINDOW_SECONDS);
					spdlog::warn("login rejected account={} reason=bad_password", MaskEmail(email));
					result["error"] = ErrorCodes::PasswdInvalid;
					return result;
				}

				//密码正确：清掉失败计数，避免历史失败影响后续登录
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
	_get_handlers.insert(make_pair(url, handler)); //将URL和处理函数插入到GET请求的处理器映射中
}

void LogicSystem::RegPost(std::string url, HttpHandler handler) {
	_post_handlers.insert(make_pair(url, handler)); //将URL和处理函数插入到POST请求的处理器映射中
}

bool LogicSystem::HandleGet(std::string path, std::shared_ptr<HttpConnection> con) {
	//如果没有找到对应URL的处理函数, 返回false, 让网络层返回404错误
    if (_get_handlers.find(path) == _get_handlers.end()) {
        return false;
    }
	_get_handlers[path](con); //调用对应URL的处理函数, 处理请求
    return true;
}

bool LogicSystem::HandlePost(std::string path, std::shared_ptr<HttpConnection> con) {
	//如果没有找到对应URL的处理函数, 返回false, 让网络层返回404错误
	if (_post_handlers.find(path) == _post_handlers.end()) {
		return false;
	}

	_post_handlers[path](con); //调用对应URL的处理函数, 处理请求
	return true;
}
