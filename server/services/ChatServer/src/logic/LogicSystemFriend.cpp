#include "LogicSystem.h"
#include "UserCache.h"
#include "MysqlDao.h"
#include "JsonUtil.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "SessionRouter.h"
#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include <cctype>
#include <charconv>
#include "Log.h"

namespace {
//加好友双方各自的第一句：申请方留言为空时用默认值，同意方固定发通过语
constexpr const char* kDefaultFriendWelcome = "你好，很高兴认识你";
constexpr const char* kDefaultApproveWelcome = "好友验证通过了，随时可以找我聊天";
} // namespace

void LogicSystem::SearchInfo(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const std::string query = root["uid"].asString(); //纯数字按 uid，否则按用户名

	Json::Value rtvalue;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_SEARCH_USER_RSP);
	});

	//搜索只返回公开资料：邮箱属于账号信息，不随搜索结果下发
	UserInfo target_info;
	bool found = false;
	if (isPureDigit(query)) {
		int target_uid = 0;
		auto [end, error] = std::from_chars(query.data(), query.data() + query.size(), target_uid);
		if (error == std::errc{} && end == query.data() + query.size() && target_uid > 0)
			found = usercache::Get(target_uid, target_info);
	}
	else {
		found = usercache::GetByName(query, target_info);
	}
	if (!found) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = target_info.uid;
	rtvalue["name"] = target_info.name;
	rtvalue["nick"] = target_info.nick;
	rtvalue["desc"] = target_info.desc;
	rtvalue["sex"] = target_info.sex;
	rtvalue["icon"] = target_info.icon;

	//关系状态由服务端给出：客户端本地缓存里的"好友"可能早已被对方删除，
	//只有库里两行都在才算好友，否则搜索页会一直显示"发消息"而无法重新申请
	const int me = session->GetUserId();
	if (me > 0) {
		rtvalue["relation"] = target_info.uid == me ? -1
			: MysqlDao::GetInstance()->GetFriendRelation(me, target_info.uid);
	}
}

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	//从解析后的Json对象中获取申请添加好友的相关信息
	auto uid = root["uid"].asInt();
	std::string welcome = root["applyname"].asString();
	if (welcome.empty())
		welcome = kDefaultFriendWelcome;
	auto bakname = root["bakname"].asString();
	auto touid = root["touid"].asInt();
	//标签缺省时是空数组; 申请通过后这些标签直接成为申请方的分组
	std::vector<std::string> tags;
	for (const auto& tag : root["tags"])
		tags.push_back(tag.asString());

	Json::Value rtvalue; //创建Json值对象，用于存储返回给客户端的数据
	rtvalue["error"] = ErrorCodes::Success; //默认返回成功状态码
	Defer defer([&rtvalue, session]() {
		//将Json值对象转换为字符串形式，准备发送给客户端
		std::string return_str = JsonToCompact(rtvalue);
		//通过会话对象发送消息给客户端, 消息ID为申请添加好友回包
		session->Send(return_str, ID_ADD_FRIEND_RSP);
		});

	//目标必须存在；已经是好友时按幂等成功处理，避免重复申请再写一遍欢迎语
	UserInfo target_info;
	if (!usercache::Get(touid, target_info)) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (MysqlDao::GetInstance()->IsMutualFriend(uid, touid))
		return;

    if (!MysqlDao::GetInstance()->AddFriendApply(uid, touid, welcome, bakname, tags)) {
        rtvalue["error"] = ErrorCodes::RPCFailed;
        return;
    }

	//目标用户未在线时不推送：申请已经写入数据库，对方登录后能在申请列表里看到
	std::string to_ip_value;
	if (!SessionRouter::Inst().Lookup(touid, to_ip_value)) {
		LOG_DEBUG << "[friend] apply stored, target offline uid=" << touid;
		return;
	}
	LOG_DEBUG << "[friend] apply uid=" << uid << " target=" << touid << " route=" << to_ip_value;

	//从配置文件中获取自身服务器的名称
	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];

	UserInfo apply_info;
	const bool b_info = usercache::Get(uid, apply_info);

	//如果目标用户在当前服务器上
	if (to_ip_value == self_name) {
		auto session = UserMgr::GetInstance()->GetSession(touid); //从UserMgr中获取目标用户的会话对象
		//如果目标用户的会话对象存在，说明目标用户在线，直接发送通知给对方
		if (!session) {
			//路由说有会话、本机却找不到：缓存与真实状态不一致，失效后下次重新问
			LOG_DEBUG << "[friend] route hit but session missing uid=" << touid;
			SessionRouter::Inst().Invalidate(touid);
			return;
		}
		Json::Value notify; //创建Json值对象，用于存储通知信息
		notify["error"] = ErrorCodes::Success;
		notify["applyuid"] = uid;
		notify["name"] = b_info ? apply_info.name : std::to_string(uid);
		notify["desc"] = welcome;
		if (b_info) {
			notify["icon"] = apply_info.icon;
			notify["sex"] = apply_info.sex;
			notify["nick"] = apply_info.nick;
		}
		std::string return_str = JsonToCompact(notify);
		//通过会话对象发送消息给目标用户, 消息ID为通知添加好友请求
		LOG_DEBUG << "[friend] notify local uid=" << touid;
		session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
		return;
	}

	AddFriendReq add_req; //创建AddFriendReq消息对象，准备发送通知
	add_req.set_applyuid(uid);
	add_req.set_touid(touid);
	add_req.set_name(b_info ? apply_info.name : std::to_string(uid));
	add_req.set_desc(welcome);
	//如果获取申请人用户基本信息成功，则将申请人用户的头像、性别和昵称填充到AddFriendReq消息对象中
	if (b_info) {
		add_req.set_icon(apply_info.icon);
		add_req.set_sex(apply_info.sex);
		add_req.set_nick(apply_info.nick);
	}

	//通过ChatGrpcClient发送通知添加好友请求给目标用户所在的服务器
	ChatGrpcClient::GetInstance()->NotifyAddFriend(to_ip_value, add_req);
}

void LogicSystem::AuthFriendApply(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	auto uid = root["fromuid"].asInt();
	auto touid = root["touid"].asInt();
	auto back_name = root["back"].asString();

	Json::Value  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	UserInfo user_info;

	const bool b_info = usercache::Get(touid, user_info);
	if (b_info) {
		rtvalue["name"] = user_info.name;
		rtvalue["nick"] = user_info.nick;
		rtvalue["icon"] = user_info.icon;
		rtvalue["sex"] = user_info.sex;
		rtvalue["uid"] = touid;
	}
	else {
		rtvalue["error"] = ErrorCodes::UidInvalid;
	}


	Defer defer([&rtvalue, session]() {
		std::string return_str = JsonToCompact(rtvalue);
		session->Send(return_str, ID_AUTH_FRIEND_RSP);
		});

    if (!b_info) return;
    //DAO在同一事务中检查申请、确认状态并建立双向好友关系，只有提交成功后才通知对方
    std::string apply_desc;
    std::string peer_back; //申请方当初给通过方设的备注，回传给申请方本地显示
    std::vector<std::string> peer_tags; //申请方当初选的分组，同样回传
    //同意弹层可以直接选分组：自己这一侧的好友关系里存下来，不必等通过后再补
    std::vector<std::string> my_tags;
    for (const auto& tag : root["tags"]) {
        if (tag.isString() && !tag.asString().empty())
            my_tags.push_back(tag.asString());
    }
	bool first_approve = false;
	if (!MysqlDao::GetInstance()->AddFriend(uid, touid, back_name, my_tags, apply_desc,
        peer_back, peer_tags, first_approve)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
	//同意方自己填的备注随回包返回，客户端不必等下一次登录才看到
	rtvalue["back"] = back_name;
	//好友关系已经提交, 会话和欢迎消息属于附加内容; 写失败只记日志, 不回滚也不让审批失败
	long long thread_id = MysqlDao::GetInstance()->GetOrCreatePrivateChat(uid, touid);
	if (thread_id > 0)
		rtvalue["thread_id"] = Json::Int64(thread_id);
	else
		LOG_ERROR << "create private chat failed, friend relation kept: " << uid << " - " << touid;

	//重复同意（例如连点两次）只回成功：关系与欢迎语都已经写过，再写会造出重复消息
	if (!first_approve)
		return;

	std::vector<ChatMessageInfo> applicant_msgs;
	std::vector<ChatMessageInfo> approver_msgs;
	if (thread_id > 0) {
		//申请方没填留言时用默认句兜底, 同意方不填写, 固定发通过语
		std::string applicant_welcome = apply_desc;
		if (applicant_welcome.empty())
			applicant_welcome = kDefaultFriendWelcome;
		//client_msg_id 带上通过时间：删好友后重新通过会复用同一对 uid，
		//固定 ID 会被唯一键挡掉，申请方就再也收不到新的通过语
		const std::string stamp = std::to_string(std::time(nullptr));
		const std::string applicant_id = "friend-welcome-" + std::to_string(touid) + "-"
			+ std::to_string(uid) + "-" + stamp;
		const std::string approver_id = "friend-welcome-" + std::to_string(uid) + "-"
			+ std::to_string(touid) + "-" + stamp;
		//先写申请方的留言，再写同意方的默认句，message_id 顺序就是聊天里的显示顺序
		if (!MysqlDao::GetInstance()->SaveChatMessages(touid, uid, thread_id,
			{{applicant_id, applicant_welcome, 0}}, applicant_msgs))
			LOG_ERROR << "save applicant welcome failed: " << applicant_id;
		if (!MysqlDao::GetInstance()->SaveChatMessages(uid, touid, thread_id,
			{{approver_id, kDefaultApproveWelcome, 0}}, approver_msgs))
			LOG_ERROR << "save approver welcome failed: " << approver_id;
	}

	//同一条欢迎消息对两个客户端的方向不同: 自己发的算已发送, 对方发的算未读
	auto append_welcome = [&](Json::Value& array, const ChatMessageInfo& message, int status) {
		Json::Value item;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["message_id"] = Json::Int64(message.message_id);
		item["thread_id"] = Json::Int64(thread_id);
		item["status"] = status;
		item["fromuid"] = message.sender_id;
		item["touid"] = message.recv_id;
		item["time"] = Json::Int64(message.created_at); //欢迎消息也参与会话排序
		array.append(item);
	};
	//同意方视角：申请方的留言是收到的未读，自己的默认句是已发送
	Json::Value approver_view(Json::arrayValue);
	for (const auto &message : applicant_msgs)
		append_welcome(approver_view, message, 3);
	for (const auto &message : approver_msgs)
		append_welcome(approver_view, message, 1);
	if (approver_view.size() > 0)
		rtvalue["welcome_msgs"] = approver_view;

	//申请方视角：自己的留言是已发送，同意方的默认句是收到的未读
	Json::Value applicant_view(Json::arrayValue);
	for (const auto &message : applicant_msgs)
		append_welcome(applicant_view, message, 1);
	for (const auto &message : approver_msgs)
		append_welcome(applicant_view, message, 3);

	//通过好友的关系已经落库，对方不在线时跳过推送，登录后拉取即可
	std::string to_ip_value;
	if (!SessionRouter::Inst().Lookup(touid, to_ip_value))
		return;

	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];
	//直接通知对方有认证通过消息
	if (to_ip_value == self_name) {
		auto session = UserMgr::GetInstance()->GetSession(touid);
		if (session) {
			//在内存中则直接发送通知对方
			Json::Value  notify;
			notify["error"] = ErrorCodes::Success;
			notify["fromuid"] = uid;
			notify["touid"] = touid;
			if (thread_id > 0)
				notify["thread_id"] = Json::Int64(thread_id);
			notify["welcome_msgs"] = applicant_view;
			notify["back"] = peer_back;
			notify["tags"] = Json::arrayValue;
			for (const auto& tag : peer_tags)
				notify["tags"].append(tag);
			UserInfo applicant;
			if (usercache::Get(uid, applicant)) {
				notify["name"] = applicant.name;
				notify["nick"] = applicant.nick;
				notify["icon"] = applicant.icon;
				notify["sex"] = applicant.sex;
			}
			else {
				notify["error"] = ErrorCodes::UidInvalid;
			}


			std::string return_str = JsonToCompact(notify);
			session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
		}
		return;
	}

	AuthFriendReq auth_req;
	auth_req.set_fromuid(uid);
	auth_req.set_touid(touid);
	auth_req.set_back(peer_back);
	for (const auto& tag : peer_tags)
		auth_req.add_tags(tag);
	if (thread_id > 0)
		auth_req.set_thread_id(thread_id);
	for (const auto &item : applicant_view) {
		auto *welcome = auth_req.add_welcome_msgs();
		welcome->set_msgid(item["msgid"].asString());
		welcome->set_msgcontent(item["content"].asString());
		welcome->set_message_id(item["message_id"].asInt64());
		welcome->set_status(item["status"].asInt());
		welcome->set_fromuid(item["fromuid"].asInt());
		welcome->set_touid(item["touid"].asInt());
	}

	//发送通知
	ChatGrpcClient::GetInstance()->NotifyAuthFriend(to_ip_value, auth_req);
}

void LogicSystem::UpdateProfile(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_UPDATE_PROFILE_RSP);
		});

	//操作者只认会话绑定的uid, 请求体里不带uid, 客户端伪造就改不到别人的资料
	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	const std::string nick = root["nick"].asString();
	const std::string desc = root["desc"].asString();
	const int sex = root["sex"].asInt();
	//长度按UTF-8字符数再校验一次, 避免绕过界面直接发超长内容把列写坏
	if (Utf8CharCount(nick) > MAX_NICK_LEN || Utf8CharCount(desc) > MAX_DESC_LEN) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	if (!MysqlDao::GetInstance()->UpdateUserProfile(uid, nick, sex, desc)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}

	//缓存里保留 name/email/icon, 只覆盖本次修改的三个字段, 不再多查一次库
	UserInfo cached;
	if (usercache::Get(uid, cached)) {
		cached.nick = nick;
		cached.sex = sex;
		cached.desc = desc;
		usercache::Put(cached);
	}

	//回包带完整结果, 客户端据此刷新本地资料页与侧栏, 不必再发一次搜索
	rtvalue["uid"] = uid;
	rtvalue["nick"] = nick;
	rtvalue["sex"] = sex;
	rtvalue["desc"] = desc;

	//自己这一侧靠回包更新, 这里通知在线好友: 昵称与签名变了, 他们的列表与聊天标题跟着刷新
	NotifyFriendsProfileChanged(uid);
}

//资料变更推送给在线好友: 先查好友列表, 再按对方所在实例分发。
//不在线的好友不占一次 RPC, 他们下次登录时从 2002 回包里拿到最新资料。
void LogicSystem::NotifyFriendsProfileChanged(int uid)
{
	if (uid <= 0)
		return;

	//头像与昵称的写入口都会同步刷新缓存，这里直接读缓存，避免每次推送再查一次库
	UserInfo user_info;
	if (!usercache::Get(uid, user_info))
		return;

	std::vector<int> friend_ids;
	if (!MysqlDao::GetInstance()->GetFriendIds(uid, friend_ids) || friend_ids.empty())
		return;

	Json::Value notify;
	notify["error"] = ErrorCodes::Success;
	notify["uid"] = uid;
	notify["name"] = user_info.name;
	notify["nick"] = user_info.nick;
	notify["desc"] = user_info.desc;
	notify["sex"] = user_info.sex;
	notify["icon"] = user_info.icon;
	const std::string notify_text = JsonToCompact(notify);

	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	//一次批量查询好友位置，避免好友数 × 一次 Redis/RPC
	const auto routes = SessionRouter::Inst().LookupBatch(friend_ids);
	std::map<std::string, std::vector<int>> remote;
	for (int friend_id : friend_ids) {
		if (friend_id <= 0)
			continue;

		auto route = routes.find(friend_id);
		if (route == routes.end())
			continue; //离线好友不推送，下次登录会从 2002/3014 拿到最新资料
		const std::string& ip = route->second;

		if (ip == self_name) {
			auto session = UserMgr::GetInstance()->GetSession(friend_id);
			if (session)
				session->Send(notify_text, ID_NOTIFY_PROFILE_CHANGED);
			continue;
		}
		remote[ip].push_back(friend_id);
	}

	for (const auto& entry : remote) {
		ProfileChangedReq auth_req;
		auth_req.set_fromuid(uid);
		for (int friend_id : entry.second)
			auth_req.add_touids(friend_id);
		auth_req.set_nick(user_info.nick);
		auth_req.set_desc(user_info.desc);
		auth_req.set_icon(user_info.icon);
		auth_req.set_sex(user_info.sex);
		ChatGrpcClient::GetInstance()->NotifyProfileChanged(entry.first, auth_req);
	}
}

//修改自己给某个好友设的备注: 只写自己这一侧, 对方那边的备注不受影响
void LogicSystem::UpdateFriendRemark(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_UPDATE_FRIEND_REMARK_RSP);
		});

	const int self_uid = session->GetUserId();
	const int friend_uid = root["touid"].asInt();
	const std::string remark = root["remark"].asString();
	//标签整组替换: 客户端提交的就是编辑后的完整集合, 空数组表示清空
	std::vector<std::string> tags;
	for (const auto& tag : root["tags"])
		tags.push_back(tag.asString());
	if (self_uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	//备注与昵称共用同一个长度上限, 清空备注同样允许
	if (Utf8CharCount(remark) > MAX_NICK_LEN) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	//返回值区分“不是好友”和“数据库错误”, 前者不能落成成功回包
	const int result = MysqlDao::GetInstance()->UpdateFriendRemark(self_uid, friend_uid, remark,
		tags);
	if (result == 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (result < 0) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}

	rtvalue["touid"] = friend_uid;
	rtvalue["remark"] = remark;
	//回包带回标签, 客户端用它刷新列表与筛选条
	rtvalue["tags"] = Json::arrayValue;
	for (const auto& tag : tags)
		rtvalue["tags"].append(tag);
}

//按 uid 取好友最新资料: 昵称和个性签名会被本人随时修改, 客户端打开会话或资料页时刷新一次
void LogicSystem::GetProfile(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const int touid = root["touid"].asInt();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_GET_PROFILE_RSP);
		});

	//只查公开资料, 不返回邮箱与密码; 目标不是好友也允许, 搜索结果同样需要
	UserInfo user_info;
	if (!usercache::Get(touid, user_info)) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	rtvalue["uid"] = user_info.uid;
	rtvalue["name"] = user_info.name;
	rtvalue["nick"] = user_info.nick;
	rtvalue["desc"] = user_info.desc;
	rtvalue["sex"] = user_info.sex;
	rtvalue["icon"] = user_info.icon;
}

void LogicSystem::DeleteFriend(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const int touid = root["touid"].asInt();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = touid; //失败时也带回来，客户端才能把乐观移除的那一行恢复
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_DELETE_FRIEND_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (touid <= 0 || touid == uid) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//只删自己这一行：对方仍把你看作好友，直到他也删除
	if (!MysqlDao::GetInstance()->DeleteFriend(uid, touid)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
}

//群事件广播：本实例成员直接投递，其他实例按路由查询结果聚合后一次转发。

bool LogicSystem::isPureDigit(const std::string& str)
{
    if (str.empty()) return false;
	for (unsigned char c : str) {
		//如果字符串中的某个字符不是数字，则返回false，说明字符串不是纯数字
		if (!std::isdigit(c)) {
			return false;
		}
	}
	return true;
}

bool LogicSystem::GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>>& list)
{
	//从mysql获取好友申请列表
	return MysqlDao::GetInstance()->GetApplyList(to_uid, list, 0, 10);
}

bool LogicSystem::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>>& user_list)
{
	//从mysql获取好友列表
	return MysqlDao::GetInstance()->GetFriendList(self_id, user_list);
}
