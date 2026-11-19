#pragma once
// 用户资料的缓存读取：先查 Redis，未命中查库并回填
//
// 登录回包、好友列表、群成员与跨实例转发读的是同一份缓存，
// 字段集合必须与 GateServer / ResourceServer 写入缓存时保持一致；
// 任何一处改字段都要同时改这里，否则会出现"有的接口带 icon、有的不带"。

#include <memory>
#include <string>

struct UserInfo;

namespace usercache {

//base_key 是 USER_BASE_INFO + uid；用户不存在时返回 false
bool GetBaseInfo(const std::string& base_key, int uid, std::shared_ptr<UserInfo>& userinfo);

} // namespace usercache
