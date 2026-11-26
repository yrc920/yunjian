// 单聊主链路的端到端用例：登录 → 会话列表 → 翻页 → 发消息 → 历史 → 已读 → 撤回 → 心跳
//
// 与单元测试的分工：这里不检查校验规则的细节，只确认「一条真实连接能不能把
// 业务跑通」，覆盖的是 TCP 帧、会话注册、DAO、Redis 令牌之间是否接得上。
//
// 前置条件：本地栈已启动，且测试账号在库里有至少一个单聊会话（由洗库快照提供）。

#include "TestClient.h"
#include "TestSupport.h"
#include "const.h"

#include <json/json.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace testclient;

//登录 ID 在 TestClient.h 里是常量（两份 const.h 不能共存于同一目标），
//这里用协议枚举校验一次，改动 ID 时编译期就会报错
static_assert(kChatLoginReq == MSG_CHAT_LOGIN, "登录请求 ID 与协议枚举不一致");
static_assert(kChatLoginRsp == MSG_CHAT_LOGIN_RSP, "登录回包 ID 与协议枚举不一致");

namespace {

//每次运行用不同的消息标识：幂等键 (thread_id, sender_id, client_msg_id) 重复会被判重
std::string UniqueMsgId(const char* prefix)
{
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	return std::string(prefix) + "-" + std::to_string(now);
}

} // namespace

int main(int argc, char** argv)
{
	Options options;
	if (!ParseOptions(argc, argv, options))
		return 1;

	//服务没起时返回 77：CTest 把这条记为 Skipped 而不是失败，
	//「忘记启动本地栈」和「服务有 bug」在报告里能区分开
	if (!PortOpen(options.gate)) {
		std::cout << "SKIP: 网关未监听 " << options.gate.host << ":"
			<< options.gate.port << "，本地栈未启动\n";
		return 77;
	}

	std::string error;
	LoginResult login;
	REQUIRE(HttpLogin(options.gate, options.email, options.passwd, login, error));
	std::cout << "登录成功 uid=" << login.uid << " chat=" << login.chat.host
		<< ":" << login.chat.port << "\n";

	ChatConn conn;
	REQUIRE(conn.Connect(login.chat, error));

	// ---------- 登录 ----------
	Json::Value response;
	REQUIRE(conn.Login(login, response, error));
	const auto& threads = response["thread_list"];
	//空列表说明测试账号没有预备数据，后面的翻页与发消息无从谈起
	REQUIRE(threads.isArray() && !threads.empty());

	//会话列表按最近活跃时间倒序，前端直接铺，不再排序
	bool ordered = true;
	for (Json::ArrayIndex i = 0; i + 1 < threads.size(); ++i) {
		if (Int64Field(threads[i]["time"]) < Int64Field(threads[i + 1]["time"]))
			ordered = false;
	}
	CHECK(ordered);

	// ---------- 翻页 ----------
	//复合游标：同一条记录不能在两页里重复出现
	int peer = 0;
	Json::Value peer_thread;
	for (const auto& thread : threads) {
		if (IntField(thread["thread_type"]) == 1 && IntField(thread["uid"]) > 0) {
			peer = IntField(thread["uid"]);
			peer_thread = thread;
			break;
		}
	}
	REQUIRE(peer > 0);

	Json::Value page_request;
	page_request["fromuid"] = login.uid;
	page_request["thread_id"] = threads[threads.size() - 1]["thread_id"];
	page_request["last_time"] = threads[threads.size() - 1]["time"];
	REQUIRE(conn.Send(ID_LOAD_CHAT_THREAD_REQ, page_request, error));
	Json::Value page;
	REQUIRE(conn.RecvUntil(ID_LOAD_CHAT_THREAD_RSP, page, error));
	CHECK_EQ(IntField(page["error"]), 0);
	//第二页的 thread 不能与第一页重合
	bool overlapped = false;
	for (const auto& item : page["thread_list"]) {
		for (const auto& first : threads) {
			if (item["thread_id"] == first["thread_id"])
				overlapped = true;
		}
	}
	CHECK(!overlapped);

	// ---------- 发文本消息 ----------
	const std::string msgid = UniqueMsgId("ctest-text");
	Json::Value text_request;
	text_request["fromuid"] = login.uid;
	text_request["touid"] = peer;
	text_request["thread_id"] = peer_thread["thread_id"];
	Json::Value item;
	item["msgid"] = msgid;
	item["content"] = "集成测试文本";
	text_request["text_array"].append(item);

	REQUIRE(conn.Send(ID_TEXT_CHAT_MSG_REQ, text_request, error));
	Json::Value text_response;
	REQUIRE(conn.RecvUntil(ID_TEXT_CHAT_MSG_RSP, text_response, error));
	CHECK_EQ(IntField(text_response["error"]), 0);
	REQUIRE(!text_response["text_array"].empty());
	//服务端负责分配 message_id 与时间，客户端不自己算
	const Json::Int64 message_id = Int64Field(text_response["text_array"][0]["message_id"]);
	CHECK(message_id > 0);
	CHECK_EQ(TextField(text_response["text_array"][0]["msgid"]), msgid);
	CHECK_EQ(Int64Field(text_response["thread_id"]), Int64Field(peer_thread["thread_id"]));

	// ---------- 历史消息 ----------
	Json::Value history_request;
	history_request["fromuid"] = login.uid;
	history_request["thread_id"] = peer_thread["thread_id"];
	history_request["message_id"] = 0;
	REQUIRE(conn.Send(ID_LOAD_CHAT_MSG_REQ, history_request, error));
	Json::Value history;
	REQUIRE(conn.RecvUntil(ID_LOAD_CHAT_MSG_RSP, history, error));
	CHECK_EQ(IntField(history["error"]), 0);
	CHECK(history["text_array"].isArray());

	//刚发的那条应该在最新一页里
	bool found = false;
	for (const auto& msg : history["text_array"]) {
		if (Int64Field(msg["message_id"]) == message_id)
			found = true;
	}
	CHECK(found);

	// ---------- 已读上报 ----------
	Json::Value read_request;
	read_request["fromuid"] = login.uid;
	read_request["thread_id"] = peer_thread["thread_id"];
	read_request["message_id"] = message_id;
	REQUIRE(conn.Send(ID_MARK_READ_REQ, read_request, error));
	Json::Value read_response;
	REQUIRE(conn.RecvUntil(ID_MARK_READ_RSP, read_response, error));
	CHECK_EQ(IntField(read_response["error"]), 0);

	// ---------- 撤回 ----------
	//刚发的消息在 2 分钟窗口内，撤回必须成功；
	//撤回后原文案不再返回，由客户端显示「已撤回」
	Json::Value recall_request;
	recall_request["fromuid"] = login.uid;
	recall_request["message_id"] = message_id;
	REQUIRE(conn.Send(ID_RECALL_MSG_REQ, recall_request, error));
	Json::Value recall_response;
	REQUIRE(conn.RecvUntil(ID_RECALL_MSG_RSP, recall_response, error));
	CHECK_EQ(IntField(recall_response["error"]), 0);

	// ---------- 心跳 ----------
	Json::Value beat_request;
	beat_request["fromuid"] = login.uid;
	REQUIRE(conn.Send(ID_HEART_BEAT_REQ, beat_request, error));
	Json::Value beat_response;
	REQUIRE(conn.RecvUntil(ID_HEARTBEAT_RSP, beat_response, error));
	CHECK_EQ(IntField(beat_response["error"]), 0);

	// ---------- 登出 ----------
	//放在最后：登出会吊销 utoken_，同账号的后续用例必须重新登录
	Json::Value logout_request;
	logout_request["fromuid"] = login.uid;
	REQUIRE(conn.Send(ID_LOGOUT_REQ, logout_request, error));
	Json::Value logout_response;
	REQUIRE(conn.RecvUntil(ID_LOGOUT_RSP, logout_response, error));
	CHECK_EQ(IntField(logout_response["error"]), 0);

	conn.Close();
	//失败时把最后一次传输/协议错误的原文带出来：断言只打印条件，看不到 error 的内容
	if (::yunjiantest::Failures() > 0 && !error.empty())
		std::cout << "最后一个错误: " << error << "\n";
	return YUNJIAN_TEST_SUMMARY();
}
