#pragma once
// 资源通道的帧编码与文件名约束
//
// 一帧 = 4 字节头长（网络序）+ JSON 头 + 原始分片。分片直接跟在头后面，
// 上传与下载共用同一套编码，客户端按头里的 seq / last / is_last 拼装。
//
// 这里只放纯函数：不读配置、不碰 Redis、不依赖会话状态，
// 因此可以被单元测试直接调用；协议规则改动后有回归用例兜着。
//

#include <json/json.h>

#include <boost/filesystem.hpp>

#include <cctype>
#include <cstdint>
#include <string>

namespace resource_frame {

//头部是固定字段组成的对象，正常不超过 1 KB，上限用来拦住畸形帧
constexpr std::size_t MAX_HEADER_LEN = 8192;
//落盘文件名上限，与客户端选择文件时的限制保持一致
constexpr std::size_t MAX_NAME_LEN = 128;

//拆帧：header 得到 JSON 头，payload 指向 body 内部的原始分片，不复制。
//payload 的生命周期跟随 body，调用方在 body 失效前把分片落盘或转发
inline bool SplitFrame(const std::string& body, Json::Value& header, const char*& payload,
	std::size_t& payload_len)
{
	if (body.size() < 4)
		return false;
	const auto* bytes = reinterpret_cast<const unsigned char*>(body.data());
	const std::uint32_t header_len = (static_cast<std::uint32_t>(bytes[0]) << 24)
		| (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8)
		| static_cast<std::uint32_t>(bytes[3]);
	//先减后比：header_len 接近 UINT32_MAX 时 4 + header_len 会溢出，绕过检查
	if (header_len == 0 || header_len > MAX_HEADER_LEN || header_len > body.size() - 4)
		return false;
	Json::Reader reader;
	if (!reader.parse(body.substr(4, header_len), header) || !header.isObject())
		return false;
	payload = body.data() + 4 + header_len;
	payload_len = body.size() - 4 - header_len;
	return true;
}

//组帧：头按紧凑 JSON 输出，分片原样拼在后面
inline std::string BuildFrame(const Json::Value& header, const char* payload,
	std::size_t payload_len)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = ""; //紧凑输出，头部只有几百字节
	const std::string text = Json::writeString(builder, header);

	std::string frame;
	frame.reserve(4 + text.size() + payload_len);
	const auto len = static_cast<std::uint32_t>(text.size());
	frame.push_back(static_cast<char>((len >> 24) & 0xFF));
	frame.push_back(static_cast<char>((len >> 16) & 0xFF));
	frame.push_back(static_cast<char>((len >> 8) & 0xFF));
	frame.push_back(static_cast<char>(len & 0xFF));
	frame += text;
	if (payload_len > 0)
		frame.append(payload, payload_len);
	return frame;
}

//md5 是断点续传的键，必须是 32 位十六进制
inline bool IsMd5(const std::string& md5)
{
	if (md5.size() != 32)
		return false;
	for (char ch : md5) {
		if (!std::isxdigit(static_cast<unsigned char>(ch)))
			return false;
	}
	return true;
}

//落盘文件名：不允许路径分隔符、不允许以点开头，防止写到 storage 之外
inline bool IsSafeName(const std::string& name)
{
	if (name.empty() || name.size() > MAX_NAME_LEN)
		return false;
	if (name == "." || name == "..")
		return false;
	if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		return false;
	if (name.front() == '.')
		return false;
	return boost::filesystem::path(name).filename().string() == name;
}

} // namespace resource_frame
