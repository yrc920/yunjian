#pragma once
// 用户公开资料的缓存读写：先查 Redis，未命中查库并回填
//
// 登录回包、搜索、好友申请、群成员与跨实例转发读的是同一份缓存，
// 字段集合必须与 GateServer / ResourceServer 写入缓存时保持一致。
// 缓存带 1 小时 TTL：Redis 里残留的旧值不会永久压住数据库中的新资料。

#include <string>

struct UserInfo;

namespace usercache {

//按 uid 取公开资料（不含密码）；用户不存在或数据库不可用返回 false
bool Get(int uid, UserInfo& userinfo);

//按用户名取公开资料；命中后同时回填 uid 与用户名两个索引
bool GetByName(const std::string& name, UserInfo& userinfo);

//资料变更后覆盖两个索引；写失败只影响下一次回源，不影响本次请求
void Put(const UserInfo& userinfo);

} // namespace usercache
