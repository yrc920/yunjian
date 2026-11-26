// 资源通道帧编码与文件名约束的回归用例
//
// 客户端与服务端共用这套规则：4 字节头长 + JSON 头 + 原始分片。
// 这里的边界值（头长、文件名、md5）都曾经是真实故障点，改动 ResourceFrame.h
// 后必须重新跑一遍，尤其是「头长超过包体」与「头长接近 UINT32_MAX」两条。

#include "ResourceFrame.h"

#include <cstdint>
#include <initializer_list>
#include <string>

#include "TestSupport.h"

namespace {

using namespace resource_frame;

//以字节序列构造包体：头长常见前导 0x00，不能用字符串字面量直接构造 std::string
std::string Bytes(std::initializer_list<unsigned char> values)
{
	std::string out;
	out.reserve(values.size());
	for (unsigned char value : values)
		out.push_back(static_cast<char>(value));
	return out;
}

//按客户端的方式拼一个包体：4 字节大端头长 + 其余内容
std::string Frame(std::uint32_t header_len, const std::string& rest)
{
	return Bytes({
		static_cast<unsigned char>((header_len >> 24) & 0xFF),
		static_cast<unsigned char>((header_len >> 16) & 0xFF),
		static_cast<unsigned char>((header_len >> 8) & 0xFF),
		static_cast<unsigned char>(header_len & 0xFF) }) + rest;
}

//模拟客户端解析服务端回包时读取头长
std::uint32_t ReadHeaderLen(const std::string& frame)
{
	const auto* bytes = reinterpret_cast<const unsigned char*>(frame.data());
	return (static_cast<std::uint32_t>(bytes[0]) << 24)
		| (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8)
		| static_cast<std::uint32_t>(bytes[3]);
}

Json::Value Header(const std::string& name, int seq, bool last)
{
	Json::Value header;
	header["name"] = name;
	header["seq"] = seq;
	header["last"] = last ? 1 : 0;
	return header;
}

} // namespace

YUNJIAN_TEST(组帧拆帧往返一致)
{
	//含 NUL 与高位字节：分片是原始字节，任何一次按字符串处理都会截断
	const std::string payload = Bytes({ 0x00, 0x01, 0xFF, 0xFE }) + " binary " + Bytes({ 0x7F });
	const auto frame = BuildFrame(Header("a.bin", 3, true), payload.data(), payload.size());
	CHECK(frame.size() > payload.size());

	Json::Value header;
	const char* got_payload = nullptr;
	std::size_t got_len = 0;
	CHECK(SplitFrame(frame, header, got_payload, got_len));
	CHECK_EQ(got_len, payload.size());
	CHECK(std::string(got_payload, got_len) == payload);

	//头部字段原样回来：JSON 只做序列化，不做类型转换
	CHECK_EQ(header["name"].asString(), std::string("a.bin"));
	CHECK_EQ(header["seq"].asInt(), 3);
	CHECK_EQ(header["last"].asInt(), 1);
	//头长字段等于 JSON 头的字节数，不含分片
	CHECK_EQ(ReadHeaderLen(frame), static_cast<std::uint32_t>(frame.size() - payload.size() - 4));
}

YUNJIAN_TEST(空载荷的帧)
{
	const auto frame = BuildFrame(Header("empty.bin", 1, true), nullptr, 0);
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 1;
	CHECK(SplitFrame(frame, header, payload, payload_len));
	CHECK_EQ(payload_len, static_cast<std::size_t>(0));
	CHECK_EQ(header["name"].asString(), std::string("empty.bin"));
}

YUNJIAN_TEST(头部含中文时的字节长度)
{
	//头长按字节算：中文文件名是 3 字节一个字，误用字符数会让分片错位
	const auto frame = BuildFrame(Header("测试文件.bin", 2, false), "xy", 2);
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;
	CHECK(SplitFrame(frame, header, payload, payload_len));
	CHECK_EQ(header["name"].asString(), std::string("测试文件.bin"));
	CHECK_EQ(payload_len, static_cast<std::size_t>(2));
	CHECK(std::string(payload, payload_len) == "xy");
}

YUNJIAN_TEST(包体不足四字节直接拒绝)
{
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;
	CHECK(!SplitFrame(std::string(), header, payload, payload_len));
	CHECK(!SplitFrame(std::string("\x00", 1), header, payload, payload_len));
	CHECK(!SplitFrame(std::string("\x00\x00\x00", 3), header, payload, payload_len));
}

YUNJIAN_TEST(头长为零或超上限被拒)
{
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;

	//头长 0：没有 JSON 头，后续解析必然失败
	CHECK(!SplitFrame(Frame(0, "{}"), header, payload, payload_len));

	//声称 8193 字节头，超过 MAX_HEADER_LEN：正常头部只有几百字节
	CHECK(!SplitFrame(Frame(8193, std::string(8193, ' ')), header, payload, payload_len));

	//恰好等于上限仍然合法，边界不能写成 >
	CHECK(SplitFrame(Frame(8192, "{}" + std::string(8192 - 2, ' ')), header, payload, payload_len));
	CHECK_EQ(payload_len, static_cast<std::size_t>(0));
}

YUNJIAN_TEST(头长超过包体长度被拒)
{
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;

	//声称 100 字节头，包体只有 14 字节
	CHECK(!SplitFrame(Frame(100, "0123456789"), header, payload, payload_len));

	//header_len == body.size() - 4 时载荷为空，是合法边界
	const auto exact = Frame(9, R"({"seq":1})");
	CHECK_EQ(exact.size(), static_cast<std::size_t>(13));
	CHECK(SplitFrame(exact, header, payload, payload_len));
	CHECK_EQ(payload_len, static_cast<std::size_t>(0));

	//再多要一个字节就越界
	CHECK(!SplitFrame(Frame(10, R"({"seq":1})"), header, payload, payload_len));
}

YUNJIAN_TEST(头长接近UINT32_MAX时不溢出)
{
	//回归用例：早期写法是 4 + header_len > body.size()，
	//header_len = 0xFFFFFFF0 时相加回绕，检查被绕过，随后 substr 抛 out_of_range
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;
	CHECK(!SplitFrame(Frame(0xFFFFFFF0u, std::string(64, 'x')), header, payload, payload_len));
	CHECK(!SplitFrame(Frame(0xFFFFFFFFu, std::string(32, 'y')), header, payload, payload_len));
}

YUNJIAN_TEST(头部必须是JSON对象)
{
	Json::Value header;
	const char* payload = nullptr;
	std::size_t payload_len = 0;

	//数组、数字、非法 JSON 都不是头部：后续按对象取字段会全部落空
	CHECK(!SplitFrame(Frame(5, "[1,2]"), header, payload, payload_len));
	CHECK(!SplitFrame(Frame(2, "42"), header, payload, payload_len));
	CHECK(!SplitFrame(Frame(3, "{a:"), header, payload, payload_len));
	//空对象的头合法但缺字段，由业务层拒绝，拆帧本身不拦
	CHECK(SplitFrame(Frame(2, "{}"), header, payload, payload_len));
}

YUNJIAN_TEST(md5只接受三十二位十六进制)
{
	const std::string upper = "0123456789ABCDEF0123456789ABCDEF";
	const std::string lower = "0123456789abcdef0123456789abcdef";
	CHECK(IsMd5(lower));
	CHECK(IsMd5(upper));

	CHECK(!IsMd5(""));
	CHECK(!IsMd5(lower.substr(0, 31)));
	CHECK(!IsMd5(lower + "0"));

	//长度对但含非十六进制字符：写进库后永远匹配不上续传记录
	CHECK(!IsMd5(std::string(31, 'a') + "z"));
	CHECK(!IsMd5(std::string(30, 'a') + "  "));
	CHECK(!IsMd5(std::string(29, 'a') + "\xE4\xB8\xAD")); //29 + 3 字节的「中」
}

YUNJIAN_TEST(文件名不允许逃出存储目录)
{
	//正常资源名
	CHECK(IsSafeName("a.png"));
	CHECK(IsSafeName("报告.pdf"));
	CHECK(IsSafeName("a.tar.gz"));
	CHECK(IsSafeName(std::string(MAX_NAME_LEN, 'a')));

	//目录穿越与绝对路径
	CHECK(!IsSafeName("../etc/passwd"));
	CHECK(!IsSafeName(".."));
	CHECK(!IsSafeName("."));
	CHECK(!IsSafeName("/etc/passwd"));
	CHECK(!IsSafeName("a/b.png"));
	CHECK(!IsSafeName("a\\b.png"));

	//以点开头的隐藏文件与空名
	CHECK(!IsSafeName(".hidden"));
	CHECK(!IsSafeName(""));

	//超长：128 字节是上限，多一个字节就拒绝
	CHECK(!IsSafeName(std::string(MAX_NAME_LEN + 1, 'a')));
}

YUNJIAN_TEST_MAIN()
