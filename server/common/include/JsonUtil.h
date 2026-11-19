#pragma once
// JSON 序列化：上网络的帧与进 Redis 的缓存统一用紧凑格式
//
// toStyledString() 会输出换行与四空格缩进，一个十几个字段的对象能多出几十字节。
// 聊天帧的长度字段只有 2 字节（上限 64KB），会话列表与历史消息页本来就贴着上限，
// 缩进会提前触发 ResponseTooLarge；Redis 缓存里更是白占内存。
//

#include <json/json.h>

#include <string>

//只去掉空白，字段内容与类型不变，客户端的解析结果完全一致
inline std::string JsonToCompact(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}
