// 群聊主链路的端到端用例：建群 → 群消息 → 群历史 → 改群名 → 群资料 → 解散
//
// 群操作会写多张表（conversation / conversation_member / chat_message），
// 这里验证的是「事务提交后数据能被读回来」，而不是 SQL 本身的正确性。
//
// 用例末尾无条件解散，避免每次运行都在库里堆一个测试群。

#include "TestClient.h"
#include "TestSupport.h"
#include "const.h"

#include <json/json.h>

#include <chrono>
#include <iostream>
#include <string>

using namespace testclient;

static_assert(kChatLoginReq == MSG_CHAT_LOGIN, "登录请求 ID 与协议枚举不一致");
static_assert(kChatLoginRsp == MSG_CHAT_LOGIN_RSP, "登录回包 ID 与协议枚举不一致");

namespace {

std::string UniqueText(const char* prefix)
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

	if (!PortOpen(options.gate)) {
		std::cout << "SKIP: 网关未监听 " << options.gate.host << ":"
			<< options.gate.port << "，本地栈未启动\n";
		return 77;
	}

	std::string error;
	LoginResult login;
	REQUIRE(HttpLogin(options.gate, options.email, options.passwd, login, error));

	ChatConn conn;
	REQUIRE(conn.Connect(login.chat, error));
	Json::Value response;
	REQUIRE(conn.Login(login, response, error));

	// ---------- 建群 ----------
	//成员取会话列表里的单聊对象，保证是真实存在的用户
	int peer = 0;
	for (const auto& thread : response["thread_list"]) {
		if (IntField(thread["thread_type"]) == 1 && IntField(thread["uid"]) > 0) {
			peer = IntField(thread["uid"]);
			break;
		}
	}
	REQUIRE(peer > 0);

	const std::string group_name = UniqueText("集成测试群");
	Json::Value create_request;
	create_request["fromuid"] = login.uid;
	create_request["name"] = group_name;
	create_request["members"].append(peer);
	REQUIRE(conn.Send(ID_CREATE_GROUP_REQ, create_request, error));

	Json::Value create_response;
	REQUIRE(conn.RecvUntil(ID_CREATE_GROUP_RSP, create_response, error));
	REQUIRE(IntField(create_response["error"]) == 0);
	const Json::Int64 thread_id = Int64Field(create_response["thread_id"]);
	REQUIRE(thread_id > 0);
	//建群的人自动成为群主
	CHECK_EQ(IntField(create_response["group"]["my_role"]), 2);

	// ---------- 群消息 ----------
	//群聊的 touid 固定 0，目标由 thread_id 表示
	const std::string msgid = UniqueText("ctest-group");
	Json::Value text_request;
	text_request["fromuid"] = login.uid;
	text_request["touid"] = 0;
	text_request["thread_id"] = thread_id;
	text_request["thread_type"] = 2;
	Json::Value item;
	item["msgid"] = msgid;
	item["content"] = "集成测试群消息";
	text_request["text_array"].append(item);
	REQUIRE(conn.Send(ID_TEXT_CHAT_MSG_REQ, text_request, error));

	Json::Value text_response;
	REQUIRE(conn.RecvUntil(ID_TEXT_CHAT_MSG_RSP, text_response, error));
	CHECK_EQ(IntField(text_response["error"]), 0);
	REQUIRE(!text_response["text_array"].empty());
	CHECK(Int64Field(text_response["text_array"][0]["message_id"]) > 0);

	// ---------- 群历史 ----------
	Json::Value history_request;
	history_request["fromuid"] = login.uid;
	history_request["thread_id"] = thread_id;
	history_request["message_id"] = 0;
	REQUIRE(conn.Send(ID_LOAD_CHAT_MSG_REQ, history_request, error));
	Json::Value history;
	REQUIRE(conn.RecvUntil(ID_LOAD_CHAT_MSG_RSP, history, error));
	CHECK_EQ(IntField(history["error"]), 0);
	//刚建群时的系统消息 + 刚才那条，至少两条
	CHECK(history["text_array"].size() >= 2);

	// ---------- 改群名 ----------
	const std::string renamed = group_name + "B";
	Json::Value rename_request;
	rename_request["fromuid"] = login.uid;
	rename_request["thread_id"] = thread_id;
	rename_request["action"] = "name";
	rename_request["value"] = renamed;
	REQUIRE(conn.Send(ID_UPDATE_GROUP_REQ, rename_request, error));
	Json::Value rename_response;
	REQUIRE(conn.RecvUntil(ID_UPDATE_GROUP_RSP, rename_response, error));
	CHECK_EQ(IntField(rename_response["error"]), 0);

	// ---------- 群资料 ----------
	Json::Value info_request;
	info_request["fromuid"] = login.uid;
	info_request["thread_id"] = thread_id;
	REQUIRE(conn.Send(ID_GET_GROUP_INFO_REQ, info_request, error));
	Json::Value info;
	REQUIRE(conn.RecvUntil(ID_GET_GROUP_INFO_RSP, info, error));
	CHECK_EQ(IntField(info["error"]), 0);
	//改过的名字要能读回来：这一步验证 UPDATE 落在了 conversation 而不是成员表
	CHECK_EQ(TextField(info["group"]["name"]), renamed);
	//群主 + 邀请的成员
	CHECK(info["members"].size() >= 2);

	// ---------- 非群主不能解散 ----------
	//这里只验证权限位：管理员也可能存在，但群主始终是建群人自己
	CHECK_EQ(IntField(info["group"]["my_role"]), 2);

	// ---------- 解散 ----------
	Json::Value dismiss_request;
	dismiss_request["fromuid"] = login.uid;
	dismiss_request["thread_id"] = thread_id;
	dismiss_request["action"] = "dismiss";
	REQUIRE(conn.Send(ID_GROUP_MEMBER_OP_REQ, dismiss_request, error));
	Json::Value dismiss_response;
	REQUIRE(conn.RecvUntil(ID_GROUP_MEMBER_OP_RSP, dismiss_response, error));
	CHECK_EQ(IntField(dismiss_response["error"]), 0);

	//解散后按 thread_id 拉资料应当被拒：软删状态下所有群操作都要过滤
	Json::Value after_request;
	after_request["fromuid"] = login.uid;
	after_request["thread_id"] = thread_id;
	CHECK(conn.Send(ID_GET_GROUP_INFO_REQ, after_request, error));
	if (conn.RecvUntil(ID_GET_GROUP_INFO_RSP, info, error))
		CHECK(IntField(info["error"]) != 0);

	conn.Close();
	//失败时把最后一次传输/协议错误的原文带出来：断言只打印条件，看不到 error 的内容
	if (::xigutest::Failures() > 0 && !error.empty())
		std::cout << "最后一个错误: " << error << "\n";
	return XIGU_TEST_SUMMARY();
}
