// 协议校验层（ValidateChatRequest）的回归用例
//
// 这一层是所有业务请求的第一道门：它决定一条帧能不能进入 DAO。
// 校验写松了，越权请求会落到业务代码里；写紧了，正常客户端会被无理由拒绝。
// 下面的用例按「一个风险点一条用例」组织，改动校验规则时先看这里为什么会红。

#include "RequestValidation.h"
#include "JsonUtil.h"
#include "const.h"

#include <json/json.h>

#include <stdexcept>
#include <string>

#include "TestSupport.h"

namespace {

constexpr int kUid = 1004; //用例里的登录用户

//测试数据直接用 JSON 字面量写，保持与被测代码看到的一致
Json::Value Parse(const std::string& text)
{
	Json::Value root;
	if (!JsonFromString(text, root))
		throw std::runtime_error("用例数据不是合法 JSON: " + text);
	return root;
}

} // namespace

YUNJIAN_TEST(请求体必须是对象)
{
	CHECK_EQ(ValidateChatRequest(ID_HEART_BEAT_REQ, Json::Value(42), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_HEART_BEAT_REQ, Json::Value(), kUid), Error_Json);
}

YUNJIAN_TEST(未认证连接不能发业务请求)
{
	const auto request = Parse(R"({"fromuid":1004,"touid":1001})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, request, 0), TokenInvalid);
	CHECK_EQ(ValidateChatRequest(ID_LOGOUT_REQ, Parse("{}"), 0), TokenInvalid);
}

YUNJIAN_TEST(已认证连接不能重复登录)
{
	//一条连接只绑定一个用户，重复登录会让原用户的映射失去清理时机
	const auto request = Parse(R"({"uid":1004,"token":"abc"})");
	CHECK_EQ(ValidateChatRequest(MSG_CHAT_LOGIN, request, kUid), TokenInvalid);
}

YUNJIAN_TEST(登录请求的字段约束)
{
	CHECK_EQ(ValidateChatRequest(MSG_CHAT_LOGIN,
		Parse(R"({"uid":1004,"token":"utoken_1004"})"), 0), Success);
	CHECK_EQ(ValidateChatRequest(MSG_CHAT_LOGIN, Parse(R"({"uid":0,"token":"x"})"), 0),
		Error_Json);
	CHECK_EQ(ValidateChatRequest(MSG_CHAT_LOGIN, Parse(R"({"uid":1004,"token":""})"), 0),
		Error_Json);
	CHECK_EQ(ValidateChatRequest(MSG_CHAT_LOGIN, Parse(R"({"uid":1004})"), 0), Error_Json);
}

YUNJIAN_TEST(心跳只校验操作者本人)
{
	CHECK_EQ(ValidateChatRequest(ID_HEART_BEAT_REQ, Parse(R"({"fromuid":1004})"), kUid), Success);
	//冒用他人身份：请求里的 fromuid 与服务端绑定的 uid 不一致
	CHECK_EQ(ValidateChatRequest(ID_HEART_BEAT_REQ, Parse(R"({"fromuid":1001})"), kUid),
		UidInvalid);
}

YUNJIAN_TEST(单聊消息不能发给自己)
{
	const auto to_self = Parse(R"({"fromuid":1004,"touid":1004,)"
		R"("text_array":[{"msgid":"m1","content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, to_self, kUid), UidInvalid);

	const auto missing_target = Parse(R"({"fromuid":1004,)"
		R"("text_array":[{"msgid":"m1","content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, missing_target, kUid), UidInvalid);
}

YUNJIAN_TEST(文本消息的数组约束)
{
	const auto empty_array = Parse(R"({"fromuid":1004,"touid":1001,"text_array":[]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, empty_array, kUid), Error_Json);

	const auto missing_msgid = Parse(R"({"fromuid":1004,"touid":1001,)"
		R"("text_array":[{"content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, missing_msgid, kUid), Error_Json);

	const auto empty_msgid = Parse(R"({"fromuid":1004,"touid":1001,)"
		R"("text_array":[{"msgid":"","content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, empty_msgid, kUid), Error_Json);

	const auto not_object = Parse(R"({"fromuid":1004,"touid":1001,"text_array":["hi"]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, not_object, kUid), Error_Json);
}

YUNJIAN_TEST(群消息的 touid 固定为 0)
{
	//群聊的目标是 thread_id：touid=0 不能被「不能发给自己」的规则误杀
	const auto group = Parse(R"({"fromuid":1004,"touid":0,"thread_id":77,)"
		R"("thread_type":2,"text_array":[{"msgid":"g1","content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, group, kUid), Success);

	//thread 号是负数说明客户端算错了，进业务层之前就该拦下
	const auto negative = Parse(R"({"fromuid":1004,"touid":0,"thread_id":-1,)"
		R"("thread_type":2,"text_array":[{"msgid":"g1","content":"hi"}]})");
	CHECK_EQ(ValidateChatRequest(ID_TEXT_CHAT_MSG_REQ, negative, kUid), Error_Json);
}

YUNJIAN_TEST(建群成员的数量与类型)
{
	std::string too_many = R"({"fromuid":1004,"name":"群","members":[)";
	for (int i = 0; i < MAX_CREATE_GROUP_MEMBER + 1; ++i)
		too_many += (i == 0 ? "" : ",") + std::to_string(1000 + i);
	too_many += "]}";
	CHECK_EQ(ValidateChatRequest(ID_CREATE_GROUP_REQ, Parse(too_many), kUid), Error_Json);

	//0 不是合法 uid，混在成员里会让后面的成员插入事务失败
	CHECK_EQ(ValidateChatRequest(ID_CREATE_GROUP_REQ,
		Parse(R"({"fromuid":1004,"name":"群","members":[1001,0]})"), kUid), Error_Json);

	CHECK_EQ(ValidateChatRequest(ID_CREATE_GROUP_REQ,
		Parse(R"({"fromuid":1004,"members":[1001]})"), kUid), Error_Json);

	CHECK_EQ(ValidateChatRequest(ID_CREATE_GROUP_REQ,
		Parse(R"({"fromuid":1004,"name":"群","members":[1001,1002]})"), kUid), Success);
}

YUNJIAN_TEST(群操作的 action 白名单)
{
	const std::string prefix = R"({"fromuid":1004,"thread_id":77,)";

	CHECK_EQ(ValidateChatRequest(ID_UPDATE_GROUP_REQ,
		Parse(prefix + R"("action":"name","value":"新群名"})"), kUid), Success);
	//拼错的 action 不能落到默认分支去执行别的操作
	CHECK_EQ(ValidateChatRequest(ID_UPDATE_GROUP_REQ,
		Parse(prefix + R"("action":"nmae","value":"新群名"})"), kUid), Error_Json);

	//退群不需要附加字段，成员身份由会话绑定的 uid 决定
	CHECK_EQ(ValidateChatRequest(ID_GROUP_MEMBER_OP_REQ,
		Parse(prefix + R"("action":"leave"})"), kUid), Success);

	//邀请必须带非空成员数组，否则会走成一次空操作
	CHECK_EQ(ValidateChatRequest(ID_GROUP_MEMBER_OP_REQ,
		Parse(prefix + R"("action":"invite"})"), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_GROUP_MEMBER_OP_REQ,
		Parse(prefix + R"("action":"invite","members":[]})"), kUid), Error_Json);

	//踢人/设管理员必须指名目标
	CHECK_EQ(ValidateChatRequest(ID_GROUP_MEMBER_OP_REQ,
		Parse(prefix + R"("action":"remove"})"), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_GROUP_MEMBER_OP_REQ,
		Parse(prefix + R"("action":"set_admin","target":1001})"), kUid), Success);
}

YUNJIAN_TEST(好友标签的数量与字符长度)
{
	const auto no_tags = Parse(R"({"uid":1004,"touid":1001,)"
		R"("applyname":"你好","bakname":""})");
	CHECK_EQ(ValidateChatRequest(ID_ADD_FRIEND_REQ, no_tags, kUid), Success);

	std::string too_many = R"({"uid":1004,"touid":1001,"applyname":"","bakname":"","tags":[)";
	for (int i = 0; i < MAX_TAG_COUNT + 1; ++i)
		too_many += (i == 0 ? "" : ",") + std::string("\"t\"");
	too_many += "]}";
	CHECK_EQ(ValidateChatRequest(ID_ADD_FRIEND_REQ, Parse(too_many), kUid), Error_Json);

	//长度按字符数算：13 个汉字是 39 字节，但只算 13 个字符，仍超过 MAX_TAG_LEN
	std::string long_tag = "一二三四五六七八九十甲乙丙"; //13 个字
	CHECK_EQ(long_tag.size(), static_cast<std::size_t>(3 * 13));
	CHECK_EQ(ValidateChatRequest(ID_ADD_FRIEND_REQ,
		Parse(R"({"uid":1004,"touid":1001,"applyname":"","bakname":"","tags":[")"
			+ long_tag + R"("]})"), kUid), Error_Json);

	CHECK_EQ(ValidateChatRequest(ID_ADD_FRIEND_REQ,
		Parse(R"({"uid":1004,"touid":1001,"applyname":"","bakname":"","tags":[""]})"), kUid),
		Error_Json);
}

YUNJIAN_TEST(资料更新的性别取值)
{
	const auto make = [](int sex) {
		return Parse(R"({"nick":"昵称","desc":"签名","sex":)" + std::to_string(sex) + "}");
	};
	CHECK_EQ(ValidateChatRequest(ID_UPDATE_PROFILE_REQ, make(0), kUid), Success);
	CHECK_EQ(ValidateChatRequest(ID_UPDATE_PROFILE_REQ, make(2), kUid), Success);
	//枚举外的值不能写进数据库，界面按 0/1/2 渲染，出现 3 会显示成空白
	CHECK_EQ(ValidateChatRequest(ID_UPDATE_PROFILE_REQ, make(3), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_UPDATE_PROFILE_REQ, make(-1), kUid), Error_Json);
}

YUNJIAN_TEST(图片消息的文件名与md5)
{
	const std::string md5 = "0123456789abcdef0123456789abcdef";
	const auto make = [&](const std::string& name, const std::string& hash) {
		return Parse(R"({"fromuid":1004,"touid":1001,"md5":")" + hash
			+ R"(","name":")" + name + R"(","token":"t","unique_id":"u1"})");
	};

	CHECK_EQ(ValidateChatRequest(ID_IMG_CHAT_MSG_REQ, make("a.png", md5), kUid), Success);

	//name 直接参与落盘路径拼接，带分隔符就能逃出 storage 目录
	CHECK_EQ(ValidateChatRequest(ID_IMG_CHAT_MSG_REQ, make("../etc/passwd", md5), kUid),
		Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_IMG_CHAT_MSG_REQ, make("a/b.png", md5), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_IMG_CHAT_MSG_REQ, make(".hidden", md5), kUid), Error_Json);

	//md5 是续传键，长度不对说明客户端算错了，后续按它匹配会永远命不中
	CHECK_EQ(ValidateChatRequest(ID_IMG_CHAT_MSG_REQ, make("a.png", md5.substr(1)), kUid),
		Error_Json);
}

YUNJIAN_TEST(文件消息的大小与原始文件名)
{
	const auto base = std::string(R"({"fromuid":1004,"touid":1001,"msg_type":3,)"
		R"("md5":"0123456789abcdef0123456789abcdef","name":"res.bin",)"
		R"("token":"t","unique_id":"u1","file_name":"报告.pdf")");

	CHECK_EQ(ValidateChatRequest(ID_RESOURCE_CHAT_MSG_REQ,
		Parse(base + R"(,"file_size":2004})"), kUid), Success);
	//空文件没有传输意义，0 与负数都要拦
	CHECK_EQ(ValidateChatRequest(ID_RESOURCE_CHAT_MSG_REQ,
		Parse(base + R"(,"file_size":0})"), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_RESOURCE_CHAT_MSG_REQ,
		Parse(base + R"(,"file_size":-1})"), kUid), Error_Json);
	//msg_type 只接受图片(2)与文件(3)，1 是纯文本，走这条路径会缺字段
	const auto wrong_type = Parse(R"({"fromuid":1004,"touid":1001,"msg_type":1,)"
		R"("md5":"0123456789abcdef0123456789abcdef","name":"res.bin",)"
		R"("token":"t","unique_id":"u1","file_name":"报告.pdf","file_size":2004})");
	CHECK_EQ(ValidateChatRequest(ID_RESOURCE_CHAT_MSG_REQ, wrong_type, kUid), Error_Json);
}

YUNJIAN_TEST(已读与撤回的标识必须为正)
{
	CHECK_EQ(ValidateChatRequest(ID_MARK_READ_REQ,
		Parse(R"({"thread_id":77,"message_id":9})"), kUid), Success);
	//0 不是有效消息号，按它更新已读游标会把游标打回起点
	CHECK_EQ(ValidateChatRequest(ID_MARK_READ_REQ,
		Parse(R"({"thread_id":77,"message_id":0})"), kUid), Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_RECALL_MSG_REQ, Parse(R"({"message_id":0})"), kUid),
		Error_Json);
	CHECK_EQ(ValidateChatRequest(ID_RECALL_MSG_REQ, Parse(R"({"message_id":9})"), kUid),
		Success);
}

YUNJIAN_TEST(未知请求一律拒绝)
{
	//校验层是白名单：没有对应分支的 msg id 不能默认放行到业务层
	const auto request = Parse(R"({"fromuid":1004,"touid":1001})");
	CHECK_EQ(ValidateChatRequest(9999, request, kUid), Error_Json);
}

YUNJIAN_TEST_MAIN()
