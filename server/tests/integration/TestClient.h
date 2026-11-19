#pragma once
// 集成测试的客户端：登录、连 ChatServer、连 ResourceServer
//
// 只用同步 asio 与本项目自己的帧格式，不引入 beast：
// 网关的响应固定带 Content-Length 并主动关闭连接，手写 HTTP 请求足够，
// 而 beast 的头文件会把每个测试目标的编译时间拉长数倍。
//
// 这一层的职责是「把协议讲清楚」，断言留给各个 test_*.cpp。

#include <boost/asio.hpp>
#include <json/json.h>

#include <cstdint>
#include <string>
#include <vector>

namespace testclient {

namespace asio = boost::asio;
using asio::ip::tcp;

//登录用的消息 ID。这里不 include services/ChatServer/include/const.h：
//资源服务也有一份 const.h，两份都定义了 ErrorCodes 与 MSG_IDS，同一个目标里不能并存。
//各测试文件在 include 对应 const.h 后用 static_assert 校验这两个值与枚举一致
constexpr std::uint16_t kChatLoginReq = 1005;
constexpr std::uint16_t kChatLoginRsp = 1006;

//协议字段的类型并不统一：proto 把端口定义成 string，会话列表里的时间有时是
//int64 有时是字符串。JsonCpp 的 asInt() 在类型不符时直接抛 LogicError，
//所以取字段统一走下面四个函数，数字与字符串两种写法都接住。
std::string TextField(const Json::Value& value);
int IntField(const Json::Value& value, int fallback = 0);
Json::Int64 Int64Field(const Json::Value& value, Json::Int64 fallback = 0);
bool BoolField(const Json::Value& value);

//host:port，用于 --gate / --res 参数
struct Endpoint
{
	std::string host = "127.0.0.1";
	unsigned short port = 0;
};

bool ParseEndpoint(const std::string& text, Endpoint& out, std::string& error);

//端口探测：服务没起时集成测试不报失败，而是返回 77 让 CTest 记为 Skipped，
//这样「没启动本地栈」和「服务有 bug」在结果里能区分开
bool PortOpen(const Endpoint& endpoint, int timeout_ms = 800);

struct LoginResult
{
	int uid = 0;
	std::string token;
	Endpoint chat; //网关下发的 ChatServer 地址，来自 StatusServer 分配的实例
};

//调用 GateServer 的 /user_login。
//passwd 传客户端 xor 之后的形态（与服务端约定一致），不是界面里输入的明文
bool HttpLogin(const Endpoint& gate, const std::string& email, const std::string& passwd,
	LoginResult& out, std::string& error);

//ChatServer：2 字节消息ID + 2 字节长度 + JSON
class ChatConn
{
public:
	ChatConn() = default;
	~ChatConn() { Close(); }

	ChatConn(const ChatConn&) = delete;
	ChatConn& operator=(const ChatConn&) = delete;

	bool Connect(const Endpoint& endpoint, std::string& error);
	void Close();

	bool Send(std::uint16_t msg_id, const Json::Value& body, std::string& error);
	//读一帧；服务端主动推送（1019 通知等）由调用方决定是否忽略
	bool Recv(std::uint16_t& msg_id, Json::Value& body, std::string& error);
	//一直读到指定 ID 为止，最多跳过 max_skip 帧，避免协议异常时死循环
	bool RecvUntil(std::uint16_t want, Json::Value& body, std::string& error, int max_skip = 32);

	//登录并等 1006 回包；成功后这条连接才允许发业务请求
	bool Login(const LoginResult& login, Json::Value& response, std::string& error);

private:
	asio::io_context _context;
	tcp::socket _socket{ _context };
};

//ResourceServer：2 字节消息ID + 4 字节长度 + 包体
//
//包体统一是「4 字节头长 + JSON 头 + 数据」：上传与续传同步的载荷为空，
//下载时头后面接分片。客户端与服务端都只写一套拆帧逻辑。
class ResourceConn
{
public:
	ResourceConn() = default;
	~ResourceConn() { Close(); }

	ResourceConn(const ResourceConn&) = delete;
	ResourceConn& operator=(const ResourceConn&) = delete;

	bool Connect(const Endpoint& endpoint, std::string& error);
	void Close();

	//发一帧：header 序列化成 JSON 头，payload 原样跟在后面。
	//不用分片的请求（下载、续传同步）payload 传空串，包体格式与上传一致
	bool Send(std::uint16_t msg_id, const Json::Value& header, const std::string& payload,
		std::string& error);
	//直接发原始帧体：用于构造畸形帧（头长越界等），验证服务端不因此崩溃
	bool SendRaw(std::uint16_t msg_id, const std::string& body, std::string& error);

	//收一帧并拆出 JSON 头与分片；回包没有分片时 payload 为空
	bool RecvFrame(std::uint16_t& msg_id, Json::Value& header, std::string& payload,
		std::string& error);

private:
	asio::io_context _context;
	tcp::socket _socket{ _context };
};

//测试账号的默认值来自命令行，缺省时用仓库里的测试用户：
//uid 1004 / 邮箱 456@456.com / 密码 123456（客户端 xor 后是 745230）
struct Options
{
	Endpoint gate;
	Endpoint resource;
	std::string email = "456@456.com";
	std::string passwd = "745230";
	std::string storage; //ResourceServer 的 storage 目录，用于清理测试文件
};

//解析 --gate / --res / --email / --passwd / --storage，
//失败时打印用法并返回 false
bool ParseOptions(int argc, char** argv, Options& out);

} // namespace testclient
