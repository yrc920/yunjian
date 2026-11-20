// 协议边界与取值转换的回归用例
//
// 三类容易「悄悄写错、错了很难查」的地方：
//   1. 字符计数：数据库列宽按字符，std::string::size() 按字节，中文差三倍
//   2. 发包组帧：ID 与长度都必须是网络序，长度字段只有 2 字节
//   3. 结果集取值：id 列是 unsigned，直接按 int64 读会抛 bad_value_access

#include "MsgNode.h"
#include "MysqlSession.h"
#include "const.h"

#include <boost/mysql/field_view.hpp>
#include <boost/mysql/string_view.hpp>

#include <cstdint>
#include <string>

#include "TestSupport.h"

XIGU_TEST(UTF8字符计数按首字节统计)
{
	CHECK_EQ(Utf8CharCount(""), static_cast<std::size_t>(0));
	CHECK_EQ(Utf8CharCount("hello"), static_cast<std::size_t>(5));

	//一个汉字 3 字节，但只算 1 个字符
	CHECK_EQ(Utf8CharCount("你好"), static_cast<std::size_t>(2));
	CHECK_EQ(std::string("你好").size(), static_cast<std::size_t>(6));

	//emoji 是 4 字节：F0 开头的那一个字节计入，后面三个 10xxxxxx 跳过
	CHECK_EQ(Utf8CharCount("\xF0\x9F\x98\x80"), static_cast<std::size_t>(1));

	//混排：2 个汉字 + 1 个 emoji + 3 个 ASCII。
	//后半段必须拆成独立字面量，否则 \x80 会把后面的 a 当十六进制位吞掉
	CHECK_EQ(Utf8CharCount("你好\xF0\x9F\x98\x80" "abc"), static_cast<std::size_t>(6));
}

XIGU_TEST(协议号分段且请求响应相邻)
{
	//2xxx 连接、3xxx 好友、4xxx 消息、5xxx 群聊；1000 段只留 HTTP 与错误码
	CHECK(MSG_CHAT_LOGIN >= 2000 && MSG_CHAT_LOGIN < 3000);
	CHECK(ID_SEARCH_USER_REQ >= 3000 && ID_SEARCH_USER_REQ < 4000);
	CHECK(ID_TEXT_CHAT_MSG_REQ >= 4000 && ID_TEXT_CHAT_MSG_REQ < 5000);
	CHECK(ID_CREATE_GROUP_REQ >= 5000 && ID_CREATE_GROUP_REQ < 6000);

	//分发层用 ResponseId(request) 回包，所有登记的请求都必须满足这条约定
	CHECK_EQ(ResponseId(MSG_CHAT_LOGIN), MSG_CHAT_LOGIN_RSP);
	CHECK_EQ(ResponseId(ID_TEXT_CHAT_MSG_REQ), ID_TEXT_CHAT_MSG_RSP);
	CHECK_EQ(ResponseId(ID_CREATE_GROUP_REQ), ID_CREATE_GROUP_RSP);
	CHECK_EQ(ResponseId(ID_LOGOUT_REQ), ID_LOGOUT_RSP);
	CHECK(MAX_REQUEST_ID > ID_NOTIFY_GROUP_CHANGED);
}

XIGU_TEST(发包时必须转网络序)
{
	const std::uint16_t msg_id = ID_TEXT_CHAT_MSG_REQ; //4007 = 0x0FA7
	SendNode node("hello", 5, msg_id);
	CHECK_EQ(node._total_len, static_cast<std::size_t>(5 + HEAD_TOTAL_LEN));

	const auto* bytes = reinterpret_cast<const unsigned char*>(node._data);
	//大端读回：高字节在前
	CHECK_EQ(static_cast<int>((bytes[0] << 8) | bytes[1]), static_cast<int>(msg_id));
	CHECK_EQ(static_cast<int>((bytes[2] << 8) | bytes[3]), 5);
	CHECK(std::string(node._data + HEAD_TOTAL_LEN, 5) == "hello");
}

XIGU_TEST(长度字段的2字节上限不被截断)
{
	//MAX_RESPONSE_LEN = 65535 - 4，是 2 字节长度字段能承载的最大包体；
	//再大就必须回错误包而不是截断后发出去
	const std::string body(MAX_RESPONSE_LEN, 'x');
	SendNode node(body.data(), body.size(), ID_TEXT_CHAT_MSG_RSP);

	const auto* bytes = reinterpret_cast<const unsigned char*>(node._data);
	CHECK_EQ(static_cast<int>((bytes[2] << 8) | bytes[3]), 65531);
	CHECK_EQ(node._total_len, static_cast<std::size_t>(65535));
	//包头之后的内容完整，没有被长度字段的窄化切断
	CHECK(std::string(node._data + HEAD_TOTAL_LEN, body.size()) == body);
}

XIGU_TEST(空包体只发包头)
{
	SendNode node("", 0, ID_HEARTBEAT_RSP);
	const auto* bytes = reinterpret_cast<const unsigned char*>(node._data);
	CHECK_EQ(node._total_len, static_cast<std::size_t>(HEAD_TOTAL_LEN));
	CHECK_EQ(static_cast<int>((bytes[0] << 8) | bytes[1]),
		static_cast<int>(ID_HEARTBEAT_RSP));
	CHECK_EQ(static_cast<int>((bytes[2] << 8) | bytes[3]), 0);
}

XIGU_TEST(整型列按实际kind取值)
{
	//signed 与 unsigned 都要能读：库里 id 列多是 unsigned，
	//早期统一按 as_int64() 取值会在 uid 上抛 bad_value_access
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(std::int64_t(42))), std::int64_t(42));
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(std::int64_t(-7))), std::int64_t(-7));
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(std::uint64_t(1004))), std::int64_t(1004));

	const std::uint64_t big = 18446744073709551615ull;
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(big)), std::int64_t(-1));
}

XIGU_TEST(空值与不可转换的列按零处理)
{
	//NULL 不能抛异常：COUNT(*) 之外的聚合、可空外键都会读到 NULL
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view()), std::int64_t(0));

	//字符串列按字面量解析，非数字得到 0 而不是异常
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(boost::mysql::string_view("123"))),
		std::int64_t(123));
	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(boost::mysql::string_view("abc"))),
		std::int64_t(0));

	CHECK_EQ(MysqlFieldToInt64(boost::mysql::field_view(1.9)), std::int64_t(1));
}

XIGU_TEST(字符串列转换保留空值语义)
{
	CHECK_EQ(MysqlFieldToString(boost::mysql::field_view()), std::string());
	CHECK_EQ(MysqlFieldToString(boost::mysql::field_view(boost::mysql::string_view("xigu"))),
		std::string("xigu"));
	//整型列按十进制输出，调用方不必区分列的原始类型
	CHECK_EQ(MysqlFieldToString(boost::mysql::field_view(std::int64_t(1004))),
		std::string("1004"));
}

XIGU_TEST_MAIN()
