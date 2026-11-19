#include "UserCache.h"
#include "JsonUtil.h"
#include "MysqlDao.h"
#include "RedisMgr.h"
#include "data.h"

#include <json/json.h>

bool usercache::GetBaseInfo(const std::string& base_key, int uid,
	std::shared_ptr<UserInfo>& userinfo)
{
	if (!userinfo)
		userinfo = std::make_shared<UserInfo>();

	std::string info_str;
	if (RedisMgr::GetInstance()->Get(base_key, info_str)) {
		Json::Reader reader;
		Json::Value root;
		reader.parse(info_str, root);
		userinfo->uid = root["uid"].asInt();
		userinfo->name = root["name"].asString();
		userinfo->email = root["email"].asString();
		userinfo->nick = root["nick"].asString();
		userinfo->desc = root["desc"].asString();
		userinfo->sex = root["sex"].asInt();
		userinfo->icon = root["icon"].asString();
		return true;
	}

	//缓存未命中才查库；查不到说明 uid 无效
	auto from_db = MysqlDao::GetInstance()->GetUser(uid);
	if (!from_db)
		return false;
	*userinfo = *from_db;

	Json::Value root;
	root["uid"] = userinfo->uid;
	root["name"] = userinfo->name;
	root["email"] = userinfo->email;
	root["nick"] = userinfo->nick;
	root["desc"] = userinfo->desc;
	root["sex"] = userinfo->sex;
	root["icon"] = userinfo->icon;
	RedisMgr::GetInstance()->Set(base_key, JsonToCompact(root));
	return true;
}
