#include "LogicSystem.h"
#include "MysqlDao.h"
#include "JsonUtil.h"
#include "UserMgr.h"
#include "SessionRouter.h"
#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include "Log.h"

void LogicSystem::BroadcastGroupEvent(long long thread_id, const std::vector<int>& members,
	int except_uid, const Json::Value& payload, int notify_id)
{
	if (members.empty())
		return;
	//本地直投与跨实例转发整体交给投递线程：群成员里只要有一个实例不可达，
	//同步 RPC 就会把业务线程按住数秒。payload/members 按值捕获，调用方返回后仍然有效
	PostDeliveryTask([this, thread_id, members, except_uid, payload, notify_id]() {
		BroadcastGroupEventNow(thread_id, members, except_uid, payload, notify_id);
	});
}

void LogicSystem::BroadcastGroupEventNow(long long thread_id, const std::vector<int>& members,
	int except_uid, const Json::Value& payload, int notify_id)
{
	if (members.empty())
		return;
	const std::string text = JsonToCompact(payload);
	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	//一次批量查询所有成员的在线位置：群上限 200，避免 200 次单点查询
	const auto routes = SessionRouter::Inst().LookupBatch(members);
	std::map<std::string, std::vector<int>> remote;

	for (int uid : members) {
		if (uid <= 0 || uid == except_uid)
			continue;
		auto route = routes.find(uid);
		if (route == routes.end())
			continue;
		const std::string& instance = route->second;
		if (instance == self_name) {
			auto peer = UserMgr::GetInstance()->GetSession(uid);
			if (peer)
				peer->Send(text, static_cast<short>(notify_id));
		}
		else {
			remote[instance].push_back(uid);
		}
	}

	for (const auto& [instance, uids] : remote) {
		GroupEventReq req;
		req.set_thread_id(thread_id);
		req.set_notify_id(notify_id);
		req.set_payload(text);
		for (int uid : uids)
			req.add_touids(uid);
		ChatGrpcClient::GetInstance()->NotifyGroupEvent(instance, req);
	}
}

//群资料或成员变化：event 决定客户端怎么处理
//info 改名称/公告，members 人数与角色变化，created 新群，removed 我被移出，dismissed 群解散
void LogicSystem::PushGroupChanged(long long thread_id, const std::vector<int>& members,
	int except_uid, const std::string& event, const Json::Value& detail)
{
	Json::Value payload;
	payload["error"] = ErrorCodes::Success;
	payload["thread_id"] = Json::Int64(thread_id);
	payload["thread_type"] = static_cast<int>(ThreadType::Group);
	payload["event"] = event;
	//detail 里放本次变化涉及的字段，客户端按 event 取用，不再逐个事件定义协议
	for (const auto& key : detail.getMemberNames())
		payload[key] = detail[key];
	BroadcastGroupEvent(thread_id, members, except_uid, payload, ID_NOTIFY_GROUP_CHANGED);
}

void LogicSystem::CreateGroup(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	bool replied = false; //成功路径会先回包再广播，错误路径仍由 Defer 兜底
	Defer defer([&response, session, &replied]() {
		if (!replied)
			session->Send(JsonToCompact(response), ID_CREATE_GROUP_RSP);
	});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}

	std::string name = root["name"].asString();
	//去掉首尾空白：只有空格的群名没有意义，客户端也会按空串回落到默认名
	const auto begin = name.find_first_not_of(" \t\r\n");
	const auto end = name.find_last_not_of(" \t\r\n");
	name = begin == std::string::npos ? "" : name.substr(begin, end - begin + 1);
	if (name.empty())
		name = "群聊";
	if (Utf8CharCount(name) > MAX_GROUP_NAME_LEN) {
		response["error"] = ErrorCodes::Error_Json;
		response["message"] = "群名称过长";
		return;
	}

	std::vector<int> members;
	if (root["members"].isArray()) {
		for (const auto& item : root["members"]) {
			const int member = item.asInt();
			if (member > 0)
				members.push_back(member);
		}
	}
	if (static_cast<int>(members.size()) > MAX_CREATE_GROUP_MEMBER) {
		response["error"] = ErrorCodes::GroupFull;
		response["message"] = "一次最多选择 50 位好友";
		return;
	}

	GroupInfo info;
	std::vector<GroupMemberInfo> member_list;
	int error = ErrorCodes::Success;
	const long long thread_id = MysqlDao::GetInstance()->CreateGroup(uid, name, members,
		info, member_list, error);
	if (thread_id <= 0) {
		response["error"] = error;
		response["message"] = error == ErrorCodes::NotFriend ? "只能邀请自己的好友"
			: (error == ErrorCodes::GroupFull ? "群成员已达上限" : "建群失败");
		return;
	}

	spdlog::info("group created thread={} owner={} members={}", thread_id, uid, info.member_count);
	response["thread_id"] = Json::Int64(thread_id);
	response["thread_type"] = static_cast<int>(ThreadType::Group);
	Json::Value group_json;
	group_json["thread_id"] = Json::Int64(info.thread_id);
	group_json["name"] = info.name;
	group_json["notice"] = info.notice;
	group_json["icon"] = info.icon;
	group_json["icon_owner"] = info.icon_owner;
	group_json["owner"] = info.owner_id;
	group_json["member_count"] = info.member_count;
	group_json["my_role"] = info.my_role;
	group_json["muted"] = false;
	group_json["my_nick"] = "";
	group_json["created_at"] = Json::Int64(info.created_at);
	response["group"] = group_json;
	response["members"] = Json::arrayValue;
	for (const auto& member : member_list) {
		Json::Value item;
		item["uid"] = member.uid;
		item["name"] = member.name;
		item["nick"] = member.nick;
		item["icon"] = member.icon;
		item["sex"] = member.sex;
		item["role"] = member.role;
		item["group_nick"] = member.group_nick;
		response["members"].append(item);
	}

	//先回包再广播：创建者如果先收到 5009/4009，客户端还没有这个会话，系统消息会被丢掉
	session->Send(JsonToCompact(response), ID_CREATE_GROUP_RSP);
	replied = true;

	//其他成员：把群资料与成员一起推过去，客户端据此立刻出现会话
	std::vector<int> ids;
	for (const auto& member : member_list)
		ids.push_back(member.uid);
	Json::Value detail;
	detail["group"] = group_json;
	detail["members"] = response["members"];
	PushGroupChanged(thread_id, ids, uid, "created", detail);

	//首条系统消息由服务端写，文案里带上创建者，成员翻历史时能看懂群的来历。
	//这条要发给包括群主在内的所有人：发送者本地没有这条消息
	const std::string text = [&]() {
		auto self = MysqlDao::GetInstance()->GetUser(uid);
		const std::string actor = self ? (self->nick.empty() ? self->name : self->nick)
			: std::to_string(uid);
		return actor + " 创建了群聊，快和大家打个招呼吧";
	}();
	ChatMessageInfo message;
	if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
		Json::Value payload;
		payload["error"] = ErrorCodes::Success;
		payload["thread_id"] = Json::Int64(thread_id);
		payload["thread_type"] = static_cast<int>(ThreadType::Group);
		payload["event"] = "message";
		payload["message"] = BuildMessageItem(message, 1);
		BroadcastGroupEvent(thread_id, ids, 0, payload, ID_NOTIFY_GROUP_CHANGED);
	}
}

void LogicSystem::GetGroupInfo(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(JsonToCompact(response), ID_GET_GROUP_INFO_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);

	GroupInfo info;
	std::vector<GroupMemberInfo> members;
	if (thread_id <= 0 || !MysqlDao::GetInstance()->GetGroupInfo(uid, thread_id, info, members)) {
		response["error"] = ErrorCodes::GroupNotFound;
		return;
	}

	Json::Value group_json;
	group_json["thread_id"] = Json::Int64(info.thread_id);
	group_json["name"] = info.name;
	group_json["notice"] = info.notice;
	group_json["icon"] = info.icon;
	group_json["icon_owner"] = info.icon_owner;
	group_json["owner"] = info.owner_id;
	group_json["member_count"] = info.member_count;
	group_json["my_role"] = info.my_role;
	group_json["muted"] = info.muted;
	group_json["my_nick"] = info.my_nick;
	group_json["created_at"] = Json::Int64(info.created_at);
	//昵称、头像、性别都来自 user 表，单独带在 members 里，客户端不需要再逐个拉资料
	response["group"] = group_json;
	response["members"] = Json::arrayValue;
	for (const auto& member : members) {
		Json::Value item;
		item["uid"] = member.uid;
		item["name"] = member.name;
		item["nick"] = member.nick;
		item["icon"] = member.icon;
		item["sex"] = member.sex;
		item["role"] = member.role;
		item["group_nick"] = member.group_nick;
		response["members"].append(item);
	}
}

//群设置：name/notice 改群资料，nickname 改我在本群的昵称，mute 切换免打扰。
//群资料变更要推给其他成员，个人设置只回自己
void LogicSystem::UpdateGroup(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(JsonToCompact(response), ID_UPDATE_GROUP_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string action = root["action"].asString();
	const std::string value = root["value"].asString();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);
	response["action"] = action;

	MysqlDao::GroupField field;
	if (action == "name") {
		if (value.empty() || Utf8CharCount(value) > MAX_GROUP_NAME_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群名称需为 1-30 个字符";
			return;
		}
		field = MysqlDao::GroupField::Name;
	}
	else if (action == "notice") {
		if (Utf8CharCount(value) > MAX_GROUP_NOTICE_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群公告最多 200 个字符";
			return;
		}
		field = MysqlDao::GroupField::Notice;
	}
	else if (action == "icon") {
		//值为资源名（32 位 md5 + .png），空串表示恢复成名字回退
		if (!value.empty()) {
			const bool shape = value.size() == 36 && value.compare(32, 4, ".png") == 0
				&& std::all_of(value.begin(), value.begin() + 32,
					[](unsigned char c) { return std::isxdigit(c) != 0; });
			if (!shape) {
				response["error"] = ErrorCodes::Error_Json;
				response["message"] = "群头像格式不正确";
				return;
			}
		}
		field = MysqlDao::GroupField::Icon;
	}
	else if (action == "nickname") {
		if (Utf8CharCount(value) > MAX_GROUP_NICK_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群昵称最多 16 个字符";
			return;
		}
		field = MysqlDao::GroupField::MyNickname;
	}
	else if (action == "mute") {
		field = MysqlDao::GroupField::Muted;
	}
	else {
		response["error"] = ErrorCodes::Error_Json;
		return;
	}

	std::string applied;
	const int code = MysqlDao::GetInstance()->UpdateGroupField(thread_id, uid, field, value, applied);
	if (code != ErrorCodes::Success) {
		response["error"] = code;
		response["message"] = code == ErrorCodes::GroupDenied ? "只有群主或管理员可以修改"
			: "群不存在或你已不在群里";
		return;
	}
	response["value"] = applied;

	//群名与公告要让所有成员看到；群昵称与免打扰是个人设置，不推送
	if (action == "name" || action == "notice" || action == "icon") {
		std::vector<int> members;
		if (!MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members))
			return;
		Json::Value detail;
		detail[action] = applied;
		//群头像还要带上上传者：客户端下载时按它定位存储目录
		if (action == "icon")
			detail["icon_owner"] = uid;
		PushGroupChanged(thread_id, members, uid, "info", detail);

		//系统消息留痕：改群名与改公告都写一行，成员翻历史时能看懂发生了什么
		auto self = MysqlDao::GetInstance()->GetUser(uid);
		const std::string actor = self ? (self->nick.empty() ? self->name : self->nick)
			: std::to_string(uid);
		const std::string text = action == "name"
			? actor + " 修改群名为「" + applied + "」"
			: action == "icon" ? actor + " 更换了群头像"
			: actor + " 更新了群公告";
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
	}
}

//成员操作：邀请、移除、设/撤管理员、退群、解散。
//每个动作成功后都会写一条系统消息并广播"成员数变化"，让所有在线成员的资料页同步
void LogicSystem::GroupMemberOp(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(JsonToCompact(response), ID_GROUP_MEMBER_OP_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string action = root["action"].asString();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);
	response["action"] = action;

	auto userName = [](int member_uid) {
		auto user = MysqlDao::GetInstance()->GetUser(member_uid);
		if (!user)
			return std::string("成员");
		return user->nick.empty() ? user->name : user->nick;
	};

	if (action == "invite") {
		std::vector<int> invited;
		if (root["members"].isArray()) {
			for (const auto& item : root["members"]) {
				if (item.asInt() > 0)
					invited.push_back(item.asInt());
			}
		}
		if (invited.empty()) {
			response["error"] = ErrorCodes::Error_Json;
			return;
		}
		std::vector<int> added;
		int member_count = 0;
		const int code = MysqlDao::GetInstance()->InviteGroupMembers(thread_id, uid, invited,
			added, member_count);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupFull ? "群成员已达上限"
				: (code == ErrorCodes::NotFriend ? "只能邀请自己的好友" : "邀请失败");
			return;
		}
		response["member_count"] = member_count;
		response["added"] = Json::arrayValue;
		std::string names;
		for (int member : added) {
			response["added"].append(member);
			if (!names.empty())
				names += "、";
			names += userName(member);
		}
		if (added.empty()) {
			response["message"] = "对方已经在群里";
			return;
		}

		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		const std::string text = userName(uid) + " 邀请 " + names + " 加入了群聊";
		ChatMessageInfo message;
		Json::Value detail;
		detail["member_count"] = member_count;
		PushGroupChanged(thread_id, members, uid, "members", detail);
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "remove" || action == "set_admin" || action == "unset_admin") {
		const int target = root["target"].asInt();
		if (target <= 0 || target == uid) {
			response["error"] = ErrorCodes::UidInvalid;
			return;
		}
		response["target"] = target;
		int code = ErrorCodes::Success;
		int member_count = 0;
		std::string text;
		if (action == "remove") {
			code = MysqlDao::GetInstance()->RemoveGroupMember(thread_id, uid, target, member_count);
			text = userName(uid) + " 将 " + userName(target) + " 移出群聊";
		}
		else {
			const bool admin = action == "set_admin";
			code = MysqlDao::GetInstance()->SetGroupAdmin(thread_id, uid, target, admin);
			text = userName(uid) + (admin ? " 将 " : " 取消 ") + userName(target)
				+ (admin ? " 设为管理员" : " 的管理员");
		}
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "没有操作权限"
				: "成员不存在或群已解散";
			return;
		}
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		if (action == "remove") {
			response["member_count"] = member_count;
			Json::Value detail;
			detail["member_count"] = member_count;
			PushGroupChanged(thread_id, members, uid, "members", detail);
			//被移除的人单独收到 removed，客户端据此把会话从列表里拿掉
			Json::Value removed;
			removed["target"] = target;
			std::vector<int> one{target};
			PushGroupChanged(thread_id, one, 0, "removed", removed);
		}
		else {
			Json::Value detail;
			detail["target"] = target;
			detail["role"] = action == "set_admin" ? static_cast<int>(GroupRole::Admin)
				: static_cast<int>(GroupRole::Member);
			PushGroupChanged(thread_id, members, uid, "members", detail);
		}
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "leave") {
		const int code = MysqlDao::GetInstance()->LeaveGroup(thread_id, uid);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "群主请先解散群聊" : "退群失败";
			return;
		}
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		Json::Value detail;
		detail["target"] = uid;
		if (!members.empty())
			PushGroupChanged(thread_id, members, uid, "members", detail);
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid,
			userName(uid) + " 退出了群聊", message) && !members.empty()) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "dismiss") {
		//解散是软删：成员行与消息都保留，这里取成员列表只为把变更推给他们
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		const int code = MysqlDao::GetInstance()->DismissGroup(thread_id, uid);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "只有群主可以解散群聊" : "解散失败";
			return;
		}
		spdlog::info("group dismissed thread={} owner={} members={}",
			thread_id, uid, members.size());
		Json::Value detail;
		PushGroupChanged(thread_id, members, 0, "dismissed", detail);
		return;
	}

	response["error"] = ErrorCodes::Error_Json;
}
