#include "UserCache.h"

#include "JsonUtil.h"
#include "MysqlDao.h"
#include "RedisMgr.h"
#include "const.h"
#include "data.h"

namespace {

//资料缓存不是权威数据：1 小时后重新回源，避免长期不登录的账号一直读到旧昵称
constexpr int kProfileTtlSeconds = 3600;

//缓存 JSON 与 UserInfo 的字段映射只维护这一份，登录、搜索、资料页共用
bool ParseProfile(const std::string& text, int expect_uid, UserInfo& info)
{
	Json::Value root;
	if (!JsonFromString(text, root) || !root.isObject())
		return false;

	const int uid = root["uid"].asInt();
	//缓存里的 uid 必须与请求一致：键写错或旧格式残留都按未命中处理
	if (uid <= 0 || (expect_uid > 0 && uid != expect_uid))
		return false;

	info.uid = uid;
	info.name = root["name"].asString();
	info.email = root["email"].asString();
	info.nick = root["nick"].asString();
	info.desc = root["desc"].asString();
	info.sex = root["sex"].asInt();
	info.icon = root["icon"].asString();
	return true;
}

//写缓存时只放公开字段：密码哈希不进 Redis，减少泄露面
void StoreProfile(const UserInfo& info)
{
	if (info.uid <= 0)
		return;

	Json::Value root;
	root["uid"] = info.uid;
	root["name"] = info.name;
	root["email"] = info.email;
	root["nick"] = info.nick;
	root["desc"] = info.desc;
	root["sex"] = info.sex;
	root["icon"] = info.icon;
	const std::string text = JsonToCompact(root);

	RedisMgr::GetInstance()->SetEx(USER_BASE_INFO + std::to_string(info.uid),
		text, kProfileTtlSeconds);
	if (!info.name.empty())
		RedisMgr::GetInstance()->SetEx(NAME_INFO + info.name, text, kProfileTtlSeconds);
}

} // namespace

bool usercache::Get(int uid, UserInfo& userinfo)
{
	if (uid <= 0)
		return false;

	std::string cached;
	if (RedisMgr::GetInstance()->Get(USER_BASE_INFO + std::to_string(uid), cached)
		&& ParseProfile(cached, uid, userinfo)) {
		return true;
	}

	auto from_db = MysqlDao::GetInstance()->GetUser(uid);
	if (!from_db)
		return false;
	userinfo = *from_db;
	StoreProfile(userinfo);
	return true;
}

bool usercache::GetByName(const std::string& name, UserInfo& userinfo)
{
	if (name.empty())
		return false;

	std::string cached;
	if (RedisMgr::GetInstance()->Get(NAME_INFO + name, cached)
		&& ParseProfile(cached, 0, userinfo) && userinfo.name == name) {
		return true;
	}

	auto from_db = MysqlDao::GetInstance()->GetUser(name);
	if (!from_db)
		return false;
	userinfo = *from_db;
	StoreProfile(userinfo);
	return true;
}

void usercache::Put(const UserInfo& userinfo)
{
	StoreProfile(userinfo);
}
