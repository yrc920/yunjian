#pragma once
// JSON 序列化：上网络的帧与进 Redis 的缓存统一用紧凑格式
//
// toStyledString() 会输出换行与四空格缩进，一个十几个字段的对象能多出几十字节。
// 聊天帧的长度字段只有 2 字节（上限 64KB），会话列表与历史消息页本来就贴着上限，
// 缩进会提前触发 ResponseTooLarge；Redis 缓存里更是白占内存。
//

#include <json/json.h>

#include <memory>
#include <string>

//只去掉空白，字段内容与类型不变，客户端的解析结果完全一致
inline std::string JsonToCompact(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}

//统一解析入口：Json::Reader 已废弃，各处自己拼 CharReader 又容易漏掉错误串。
//解析失败返回 false，out 不保证是对象；调用方仍需检查 isObject
inline bool JsonFromString(const std::string& text, Json::Value& out)
{
	Json::CharReaderBuilder builder;
	builder["collectComments"] = false;
	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(text.data(), text.data() + text.size(), &out, &errors);
}
