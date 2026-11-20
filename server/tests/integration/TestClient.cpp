#include "TestClient.h"
#include "JsonUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>

namespace testclient {

namespace {

//域名与 IP 都走 resolver：命令行里填 localhost 时不会因 make_address 抛异常
bool ConnectTcp(tcp::socket& socket, const Endpoint& endpoint, std::string& error)
{
	boost::system::error_code ec;
	tcp::resolver resolver(socket.get_executor());
	const auto results = resolver.resolve(endpoint.host, std::to_string(endpoint.port), ec);
	if (ec) {
		error = "解析 " + endpoint.host + " 失败: " + ec.message();
		return false;
	}
	asio::connect(socket, results, ec);
	if (ec) {
		error = "连接 " + endpoint.host + ":" + std::to_string(endpoint.port)
			+ " 失败: " + ec.message();
		return false;
	}
	return true;
}

//大端写 16 位：帧头统一用网络序，不依赖平台字节序
void AppendU16(std::string& out, std::uint16_t value)
{
	out.push_back(static_cast<char>((value >> 8) & 0xFF));
	out.push_back(static_cast<char>(value & 0xFF));
}

std::uint16_t ReadU16(const char* data)
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(data);
	return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) | bytes[1]);
}

std::uint32_t ReadU32(const char* data)
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(data);
	return (static_cast<std::uint32_t>(bytes[0]) << 24)
		| (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8)
		| static_cast<std::uint32_t>(bytes[3]);
}

std::string CompactJson(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}

bool ParseJson(const std::string& text, Json::Value& out)
{
	return JsonFromString(text, out);
}

//HTTP 响应只有两种形态：带 Content-Length 的定长体，或读到对端关闭为止。
//网关用 beast 组装响应，body 是 string，因此总是前者；后者作为兜底
bool HttpPost(const Endpoint& gate, const std::string& path, const std::string& body,
	std::string& response_body, std::string& error)
{
	asio::io_context context;
	tcp::socket socket(context);
	if (!ConnectTcp(socket, gate, error))
		return false;

	boost::system::error_code ec;
	std::string request = "POST " + path + " HTTP/1.1\r\n";
	request += "Host: " + gate.host + ":" + std::to_string(gate.port) + "\r\n";
	request += "Content-Type: application/json\r\n";
	request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
	request += "Connection: close\r\n\r\n";
	request += body;

	asio::write(socket, asio::buffer(request), ec);
	if (ec) {
		error = "发送请求失败: " + ec.message();
		return false;
	}

	std::string raw;
	std::size_t header_end = std::string::npos;
	std::size_t content_length = std::string::npos;
	char buffer[4096];
	while (true) {
		const std::size_t got = socket.read_some(asio::buffer(buffer), ec);
		if (got > 0)
			raw.append(buffer, got);
		if (header_end == std::string::npos) {
			header_end = raw.find("\r\n\r\n");
			if (header_end != std::string::npos) {
				//在响应头里找 Content-Length，大小写不敏感
				const std::string head = raw.substr(0, header_end);
				std::string lower = head;
				std::transform(lower.begin(), lower.end(), lower.begin(),
					[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
				const auto pos = lower.find("content-length:");
				if (pos != std::string::npos)
					content_length = std::strtoul(head.c_str() + pos + 15, nullptr, 10);
			}
		}
		if (header_end != std::string::npos && content_length != std::string::npos
			&& raw.size() >= header_end + 4 + content_length)
			break; //已经收到完整的定长体
		if (ec == asio::error::eof)
			break; //对端关闭：读到哪算哪
		if (ec) {
			error = "读取响应失败: " + ec.message();
			return false;
		}
	}

	if (header_end == std::string::npos) {
		error = "响应缺少 HTTP 头: " + raw.substr(0, 120);
		return false;
	}
	response_body = raw.substr(header_end + 4);
	return true;
}

} // namespace

std::string TextField(const Json::Value& value)
{
	if (value.isString())
		return value.asString();
	if (value.isInt64()) //涵盖 int / uint / int64
		return std::to_string(value.asInt64());
	if (value.isDouble())
		return std::to_string(value.asDouble());
	if (value.isBool())
		return value.asBool() ? "1" : "0";
	return {};
}

int IntField(const Json::Value& value, int fallback)
{
	if (value.isInt64())
		return static_cast<int>(value.asInt64());
	if (value.isDouble())
		return static_cast<int>(value.asDouble());
	//端口这类字段在 proto 里是 string，直接 asInt() 会抛 LogicError
	if (value.isString())
		return static_cast<int>(std::strtoll(value.asString().c_str(), nullptr, 10));
	if (value.isBool())
		return value.asBool() ? 1 : 0;
	return fallback;
}

Json::Int64 Int64Field(const Json::Value& value, Json::Int64 fallback)
{
	if (value.isInt64())
		return value.asInt64();
	if (value.isDouble())
		return static_cast<Json::Int64>(value.asDouble());
	if (value.isString())
		return std::strtoll(value.asString().c_str(), nullptr, 10);
	if (value.isBool())
		return value.asBool() ? 1 : 0;
	return fallback;
}

bool BoolField(const Json::Value& value)
{
	if (value.isBool())
		return value.asBool();
	if (value.isInt64())
		return value.asInt64() != 0;
	if (value.isString())
		return value.asString() == "1" || value.asString() == "true";
	return false;
}

bool ParseEndpoint(const std::string& text, Endpoint& out, std::string& error)
{	const auto colon = text.rfind(':');
	if (colon == std::string::npos) {
		error = "地址缺少端口: " + text + "（期望 host:port）";
		return false;
	}
	const std::string port = text.substr(colon + 1);
	if (port.empty() || port.find_first_not_of("0123456789") != std::string::npos) {
		error = "端口不是数字: " + text;
		return false;
	}
	out.host = text.substr(0, colon);
	out.port = static_cast<unsigned short>(std::strtoul(port.c_str(), nullptr, 10));
	if (out.host.empty() || out.port == 0) {
		error = "地址不完整: " + text;
		return false;
	}
	return true;
}

bool PortOpen(const Endpoint& endpoint, int timeout_ms)
{
	(void)timeout_ms; //本地回环的 connect 立即返回，参数保留给将来的远程地址
	asio::io_context context;
	tcp::socket socket(context);
	std::string error;
	if (!ConnectTcp(socket, endpoint, error))
		return false;
	boost::system::error_code ec;
	socket.close(ec);
	return true;
}

bool HttpLogin(const Endpoint& gate, const std::string& email, const std::string& passwd,
	LoginResult& out, std::string& error)
{
	Json::Value request;
	request["email"] = email;
	request["passwd"] = passwd;

	std::string body;
	if (!HttpPost(gate, "/user_login", CompactJson(request), body, error))
		return false;

	Json::Value response;
	if (!ParseJson(body, response)) {
		error = "登录响应不是 JSON: " + body.substr(0, 200);
		return false;
	}
	if (IntField(response["error"]) != 0) {
		error = "登录失败, error=" + std::to_string(IntField(response["error"]));
		return false;
	}

	out.uid = IntField(response["uid"]);
	out.token = TextField(response["token"]);
	out.chat.host = TextField(response["host"]);
	out.chat.port = static_cast<unsigned short>(IntField(response["port"]));
	if (out.uid <= 0 || out.token.empty() || out.chat.host.empty() || out.chat.port == 0) {
		error = "登录响应缺字段: " + CompactJson(response).substr(0, 200);
		return false;
	}
	return true;
}

// ---------- ChatConn ----------

bool ChatConn::Connect(const Endpoint& endpoint, std::string& error)
{
	return ConnectTcp(_socket, endpoint, error);
}

void ChatConn::Close()
{
	boost::system::error_code ec;
	_socket.close(ec);
}

bool ChatConn::Send(std::uint16_t msg_id, const Json::Value& body, std::string& error)
{
	const std::string text = CompactJson(body);
	if (text.size() > 65535) {
		error = "请求体超过 2 字节长度上限: " + std::to_string(text.size());
		return false;
	}
	std::string frame;
	frame.reserve(4 + text.size());
	AppendU16(frame, msg_id);
	AppendU16(frame, static_cast<std::uint16_t>(text.size()));
	frame += text;

	boost::system::error_code ec;
	asio::write(_socket, asio::buffer(frame), ec);
	if (ec) {
		error = "发送帧失败: " + ec.message();
		return false;
	}
	return true;
}

bool ChatConn::Recv(std::uint16_t& msg_id, Json::Value& body, std::string& error)
{
	char head[4] = {};
	boost::system::error_code ec;
	asio::read(_socket, asio::buffer(head, sizeof(head)), ec);
	if (ec) {
		error = "读帧头失败: " + ec.message();
		return false;
	}
	msg_id = ReadU16(head);
	const std::uint16_t length = ReadU16(head + 2);

	std::string text(length, '\0');
	if (length > 0) {
		asio::read(_socket, asio::buffer(text.data(), length), ec);
		if (ec) {
			error = "读帧体失败: " + ec.message();
			return false;
		}
	}
	if (length == 0) {
		body = Json::Value(Json::objectValue);
		return true;
	}
	if (!ParseJson(text, body)) {
		error = "帧体不是 JSON: " + text.substr(0, 200);
		return false;
	}
	return true;
}

bool ChatConn::RecvUntil(std::uint16_t want, Json::Value& body, std::string& error, int max_skip)
{
	for (int skipped = 0; skipped <= max_skip; ++skipped) {
		std::uint16_t msg_id = 0;
		if (!Recv(msg_id, body, error))
			return false;
		if (msg_id == want)
			return true;
		//服务端推送（好友申请、群变更等）不属于本次请求的回应，跳过继续读
	}
	error = "等不到 " + std::to_string(want) + " 回包，已跳过 " + std::to_string(max_skip) + " 帧";
	return false;
}

bool ChatConn::Login(const LoginResult& login, Json::Value& response, std::string& error)
{
	Json::Value request;
	request["uid"] = login.uid;
	request["token"] = login.token;
	if (!Send(kChatLoginReq, request, error))
		return false;
	if (!RecvUntil(kChatLoginRsp, response, error))
		return false;
	if (IntField(response["error"]) != 0) {
		error = "聊天登录失败, error=" + std::to_string(IntField(response["error"]));
		return false;
	}
	return true;
}

// ---------- ResourceConn ----------

bool ResourceConn::Connect(const Endpoint& endpoint, std::string& error)
{
	return ConnectTcp(_socket, endpoint, error);
}

void ResourceConn::Close()
{
	boost::system::error_code ec;
	_socket.close(ec);
}

bool ResourceConn::Send(std::uint16_t msg_id, const Json::Value& header,
	const std::string& payload, std::string& error)
{
	const std::string text = CompactJson(header);
	std::string body;
	body.reserve(4 + text.size() + payload.size());
	body.push_back(static_cast<char>((text.size() >> 24) & 0xFF));
	body.push_back(static_cast<char>((text.size() >> 16) & 0xFF));
	body.push_back(static_cast<char>((text.size() >> 8) & 0xFF));
	body.push_back(static_cast<char>(text.size() & 0xFF));
	body += text;
	body += payload;
	return SendRaw(msg_id, body, error);
}

bool ResourceConn::SendRaw(std::uint16_t msg_id, const std::string& body, std::string& error)
{
	const std::uint32_t body_len = static_cast<std::uint32_t>(body.size());
	std::string frame;
	frame.reserve(6 + body.size());
	AppendU16(frame, msg_id);
	frame.push_back(static_cast<char>((body_len >> 24) & 0xFF));
	frame.push_back(static_cast<char>((body_len >> 16) & 0xFF));
	frame.push_back(static_cast<char>((body_len >> 8) & 0xFF));
	frame.push_back(static_cast<char>(body_len & 0xFF));
	frame += body;

	boost::system::error_code ec;
	asio::write(_socket, asio::buffer(frame), ec);
	if (ec) {
		error = "发送资源帧失败: " + ec.message();
		return false;
	}
	return true;
}

bool ResourceConn::RecvFrame(std::uint16_t& msg_id, Json::Value& header, std::string& payload,
	std::string& error)
{
	char head[6] = {};
	boost::system::error_code ec;
	asio::read(_socket, asio::buffer(head, sizeof(head)), ec);
	if (ec) {
		error = "读资源帧头失败: " + ec.message();
		return false;
	}
	msg_id = ReadU16(head);
	const std::uint32_t body_len = ReadU32(head + 2);

	std::string body(body_len, '\0');
	if (body_len > 0) {
		asio::read(_socket, asio::buffer(body.data(), body_len), ec);
		if (ec) {
			error = "读资源帧体失败: " + ec.message();
			return false;
		}
	}
	//包体前 4 字节是 JSON 头长度，剩余部分是分片（没有分片时为空）
	if (body_len < 4) {
		error = "资源回包不足 4 字节: " + std::to_string(body_len);
		return false;
	}
	const std::uint32_t header_len = ReadU32(body.data());
	if (header_len == 0 || header_len > body_len - 4) {
		error = "资源回包头长非法: " + std::to_string(header_len)
			+ " body=" + std::to_string(body_len);
		return false;
	}
	if (!ParseJson(body.substr(4, header_len), header)) {
		error = "资源回包头不是 JSON 对象";
		return false;
	}
	payload = body.substr(4 + header_len);
	return true;
}

// ---------- 命令行 ----------

bool ParseOptions(int argc, char** argv, Options& out)
{
	out.gate = Endpoint{ "127.0.0.1", 8080 };
	out.resource = Endpoint{ "127.0.0.1", 9090 };

	for (int i = 1; i < argc; ++i) {
		const std::string key = argv[i];
		const bool has_value = i + 1 < argc;
		std::string error;
		if (key == "--gate" && has_value) {
			if (!ParseEndpoint(argv[++i], out.gate, error)) {
				std::cerr << error << "\n";
				return false;
			}
		}
		else if (key == "--res" && has_value) {
			if (!ParseEndpoint(argv[++i], out.resource, error)) {
				std::cerr << error << "\n";
				return false;
			}
		}
		else if (key == "--email" && has_value) {
			out.email = argv[++i];
		}
		else if (key == "--passwd" && has_value) {
			out.passwd = argv[++i];
		}
		else if (key == "--storage" && has_value) {
			out.storage = argv[++i];
		}
		else {
			std::cerr << "未知参数: " << key << "\n"
				<< "用法: " << argv[0]
				<< " [--gate host:port] [--res host:port] [--email 邮箱]"
				<< " [--passwd 密码] [--storage 目录]\n";
			return false;
		}
	}
	return true;
}

} // namespace testclient
