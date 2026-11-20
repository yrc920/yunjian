#include "LogicSystem.h"
#include "MysqlDao.h"
#include "JsonUtil.h"
#include "UserMgr.h"
#include "SessionRouter.h"
#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include "CServer.h"
#include <charconv>
#include <cstdint>
#include <ctime>
#include "Log.h"

namespace {
//@ 名单在请求里是 uid 数组，落库与未读查询用逗号分隔串。
//去重并忽略非正数，避免客户端传入的脏值拼进 SQL 参数
std::string JoinMentions(const Json::Value& array)
{
	std::string result;
	if (!array.isArray())
		return result;
	for (const auto& item : array) {
		//客户端历史上传过字符串形式的 uid，两种都接受，避免 @ 静默丢失
		int uid = 0;
		if (item.isInt()) {
			uid = item.asInt();
		}
		else if (item.isString()) {
			const auto text = item.asString();
			auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), uid);
			if (error != std::errc{} || end != text.data() + text.size())
				uid = 0;
		}
		if (uid <= 0)
			continue;
		const std::string text = std::to_string(uid);
		if (("," + result + ",").find("," + text + ",") != std::string::npos)
			continue;
		if (!result.empty())
			result += ",";
		result += text;
	}
	return result;
}
} // namespace

void LogicSystem::DealChatTextMsg(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	auto uid = root["fromuid"].asInt();
	auto touid = root["touid"].asInt();
	long long thread_id = root.get("thread_id", Json::Int64(0)).asInt64();

	const Json::Value  arrays = root["text_array"];

	//先把回执按"全部失败"填好, 后面写库成功后逐条改成已发送;
	//中途任何一步返回, Defer 都会把当前状态发回客户端
	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["text_array"] = Json::arrayValue;
	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	for (const auto &item : arrays) {
		Json::Value response_item;
		response_item["msgid"] = item["msgid"].asString();
		response_item["content"] = item["content"].asString();
		response_item["status"] = 2;
		rtvalue["text_array"].append(response_item);
	}
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_TEXT_CHAT_MSG_RSP);
	});
	std::vector<std::pair<std::string, std::string>> contents;
	for (const auto& item : arrays) contents.emplace_back(item["msgid"].asString(), item["content"].asString());
	//会话类型以库里为准：同一个文本通道既能写单聊消息，也能写群消息。
	//thread_id 为 0 时一定是单聊，客户端还没建会话
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	//先落库再投递: 对方离线时消息已经保存在 chat_message, 登录后还能拉回
	std::vector<ChatMessageInfo> saved;
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		if (!MysqlDao::GetInstance()->SaveGroupMessages(uid, thread_id, mentions, contents, saved)) {
			rtvalue["error"] = ErrorCodes::RPCFailed;
			rtvalue["group_invalid"] = true; //不在群里或群已解散
			return;
		}
	}
	else if (!MysqlDao::GetInstance()->SaveChatMessages(uid, touid, thread_id, contents, saved)) {
		//写库失败最常见的原因是关系不完整（任何一方删了好友）：
		//回明确的错误码，客户端按"已不是好友"提示，其余情况才是服务端故障
		rtvalue["error"] = MysqlDao::GetInstance()->IsMutualFriend(uid, touid)
			? ErrorCodes::RPCFailed : ErrorCodes::NotFriend;
		return;
	}
	//thread_id 可能是0(客户端还没建会话), 以写库结果为准回填
	if (!saved.empty()) thread_id = saved.front().thread_id;
	spdlog::debug("message saved thread={} sender={} count={} last_id={}", thread_id, uid,
		saved.size(), saved.empty() ? 0 : saved.back().message_id);
	if (session->IsClosed() || !_p_server->CheckValid(session->GetSessionId())) return;
	rtvalue["thread_id"] = Json::Int64(thread_id);
	rtvalue["thread_type"] = thread_type;
	rtvalue["text_array"] = Json::arrayValue;
	Json::Value notify = rtvalue;
	for (const auto& message : saved) {
		Json::Value item;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["thread_id"] = Json::Int64(message.thread_id);
		item["message_id"] = Json::Int64(message.message_id);
		item["status"] = 1;
		item["time"] = Json::Int64(message.created_at); //发送方按服务端时间显示
		item["mentions"] = message.mentions;
		rtvalue["text_array"].append(item);
	}
	notify["text_array"] = rtvalue["text_array"];
	//同一批消息发给接收方时状态改成未读, 与发送方看到的已发送区分
	for (auto &item : notify["text_array"])
		item["status"] = 3;

	//群聊：除自己外全部成员都要收到，本实例直投、其他实例聚合后转发
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		std::vector<int> members;
		if (!MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members))
			return;
		BroadcastGroupEvent(thread_id, members, uid, notify, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
		return;
	}

	//消息已经提交到数据库；实时投递交给投递线程。
	//对端实例不可达时 gRPC 会阻塞到截止时间，不能让唯一的业务线程陪着等
	TextChatMsgReq text_msg_req;
	text_msg_req.set_fromuid(uid);
	text_msg_req.set_touid(touid);
	text_msg_req.set_thread_id(thread_id);
	for (const auto& message : saved) {
		auto* text_msg = text_msg_req.add_textmsgs();
		text_msg->set_msgid(message.client_msg_id);
		text_msg->set_msgcontent(message.content);
		text_msg->set_message_id(message.message_id);
		text_msg->set_status(3);
		text_msg->set_fromuid(uid);
		text_msg->set_touid(touid);
		text_msg->set_time(message.created_at); //跨实例也按落库时间显示，不用转发时刻
	}

	PostDeliveryTask([req = std::move(text_msg_req), notify = std::move(notify),
		touid]() mutable {
		const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
		const std::string payload = JsonToCompact(notify);

		//返回 false 表示"缓存位置失效"：本实例没有这条会话，或对端实例找不到连接。
		//此时失效缓存并回源一次，不能直接当离线——用户刚换实例时最常走到这里
		auto deliver = [&](const std::string& instance) {
			if (instance == self_name) {
				auto peer_session = UserMgr::GetInstance()->GetSession(touid);
				if (!peer_session)
					return false;
				peer_session->Send(payload, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
				return true;
			}
			const auto forwarded = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(
				instance, req, Json::Value());
			return forwarded.error() != ErrorCodes::UidInvalid;
		};

		std::string to_ip_value;
		if (!SessionRouter::Inst().Lookup(touid, to_ip_value))
			return; //对方不在线：消息已在库里，登录同步会补发
		if (deliver(to_ip_value))
			return;

		SessionRouter::Inst().Invalidate(touid);
		std::string retry_ip;
		if (SessionRouter::Inst().Lookup(touid, retry_ip) && retry_ip != to_ip_value)
			deliver(retry_ip);
		//投递失败只影响"立刻看到"，接收方下次登录仍会用 2002 同步补回
	});
}

void LogicSystem::DealChatImgMsg(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	const int uid = root["fromuid"].asInt();
	const int touid = root["touid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string md5 = root["md5"].asString();
	const std::string name = root["name"].asString();
	const std::string unique_id = root["unique_id"].asString();

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["fromuid"] = uid;
	response["touid"] = touid;
	response["md5"] = md5;
	response["name"] = name;
	response["unique_id"] = unique_id;

	//消息先落库, content存资源文件名; 资源本体由客户端随后上传到 ResourceServer。
	//群聊按会话类型写群消息表，通知留到资源上传完成后统一广播
	ChatMessageInfo saved;
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	const bool group = thread_type == static_cast<int>(ThreadType::Group);
	const bool saved_ok = group
		? MysqlDao::GetInstance()->SaveGroupResourceMessage(uid, thread_id, mentions, unique_id,
			1, name, saved)
		: MysqlDao::GetInstance()->SaveResourceMessage(uid, touid, thread_id, unique_id, 1,
			name, saved);
	if (!saved_ok) {
		response["error"] = ErrorCodes::RPCFailed;
		response["group_invalid"] = group;
		session->Send(JsonToCompact(response), ID_IMG_CHAT_MSG_RSP);
		return;
	}

	response["thread_id"] = Json::Int64(saved.thread_id);
	response["thread_type"] = thread_type;
	response["message_id"] = Json::Int64(saved.message_id);
	response["status"] = 0; //资源尚未上传; 接收方通知留到资源完成后的章节
	response["time"] = Json::Int64(saved.created_at);
	session->Send(JsonToCompact(response), ID_IMG_CHAT_MSG_RSP);
}

//视频/文件消息只落库，资源本体由客户端收到回执后上传；视频首帧是独立资源，
//一起写进 content，接收方下载前就能用它做预览
void LogicSystem::DealResourceChatMsg(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	const int uid = root["fromuid"].asInt();
	const int touid = root["touid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	const int msg_type = root["msg_type"].asInt();
	const std::string md5 = root["md5"].asString();
	const std::string name = root["name"].asString();
	const std::string file_name = root["file_name"].asString();
	const long long file_size = root["file_size"].asInt64();
	const std::string thumb = root["thumb"].asString();
	const std::string unique_id = root["unique_id"].asString();

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["fromuid"] = uid;
	response["touid"] = touid;
	response["msg_type"] = msg_type;
	response["name"] = name;
	response["unique_id"] = unique_id;

	//content 保存接收方渲染卡片所需的元数据；资源名与原始文件名分开，
	//服务器只按资源名落盘，展示名不参与路径拼接
	Json::Value meta;
	meta["name"] = file_name;
	meta["size"] = Json::Int64(file_size);
	meta["resource"] = name;
	meta["thumb"] = thumb;
	Json::FastWriter writer;
	std::string content = writer.write(meta);
	while (!content.empty()
		&& (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
		content.pop_back();
	}

	ChatMessageInfo saved;
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	const bool group = thread_type == static_cast<int>(ThreadType::Group);
	const bool saved_ok = group
		? MysqlDao::GetInstance()->SaveGroupResourceMessage(uid, thread_id, mentions, unique_id,
			msg_type, content, saved)
		: MysqlDao::GetInstance()->SaveResourceMessage(uid, touid, thread_id, unique_id,
			msg_type, content, saved);
	if (!saved_ok) {
		response["error"] = ErrorCodes::RPCFailed;
		response["group_invalid"] = group;
		session->Send(JsonToCompact(response), ID_RESOURCE_CHAT_MSG_RSP);
		return;
	}

	response["thread_id"] = Json::Int64(saved.thread_id);
	response["thread_type"] = thread_type;
	response["message_id"] = Json::Int64(saved.message_id);
	response["status"] = 0; //资源尚未上传，等 ResourceServer 完成通知
	response["time"] = Json::Int64(saved.created_at);
	session->Send(JsonToCompact(response), ID_RESOURCE_CHAT_MSG_RSP);
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const int uid = root["fromuid"].asInt();
	const int peer_uid = root["touid"].asInt();
	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["uid"] = peer_uid;
	//打开会话时补拉一次: 登录后新加的好友可能还没有 thread_id
	const long long thread_id = MysqlDao::GetInstance()->GetOrCreatePrivateChat(uid, peer_uid);
	if (thread_id <= 0) {
		response["error"] = ErrorCodes::RPCFailed;
	} else {
		response["thread_id"] = Json::Int64(thread_id);
		auto peer = MysqlDao::GetInstance()->GetUser(peer_uid);
		if (peer) {
			response["name"] = peer->name;
			response["nick"] = peer->nick;
			response["icon"] = peer->icon;
			response["sex"] = peer->sex;
		}
	}
	session->Send(JsonToCompact(response), ID_CREATE_PRIVATE_CHAT_RSP);
}

void LogicSystem::LoadChatThreads(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const int uid = root["fromuid"].asInt();
	//客户端把上一页最后一条的 thread_id 与 time 一起发回来当复合游标；首次都为0。
	//只带 thread_id 的旧客户端仍走原来的升序游标，服务端保持兼容。
	const long long last_id = root["thread_id"].asInt64();
	const long long last_time = root.get("last_time", Json::Int64(0)).asInt64();
	std::vector<ChatThreadInfo> threads;
	bool has_more = false;
	Json::Value response;
	response["error"] = MysqlDao::GetInstance()->GetChatThreads(
		uid, last_id, 100, threads, has_more, last_time)
		? ErrorCodes::Success : ErrorCodes::RPCFailed;
	response["thread_list"] = Json::arrayValue;
	response["load_more"] = has_more;
	for (const auto& thread : threads)
		response["thread_list"].append(BuildThreadItem(thread));
	session->Send(JsonToCompact(response), ID_LOAD_CHAT_THREAD_RSP);
}

void LogicSystem::LoadChatMessages(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const int uid = root["fromuid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	//只取比游标大的消息, 历史页和实时消息因此可以按 message_id 拼成一条有序序列
	const long long last_message_id = root["message_id"].asInt64();
	std::vector<ChatMessageInfo> messages;
	bool has_more = false;
	const bool success = MysqlDao::GetInstance()->GetChatMessages(
		uid, thread_id, last_message_id, 100, messages, has_more);
	const int thread_type = ThreadTypeOf(thread_id);
	Json::Value response;
	response["error"] = success ? ErrorCodes::Success : ErrorCodes::RPCFailed;
	response["thread_id"] = Json::Int64(thread_id);
	response["thread_type"] = thread_type;
	response["load_more"] = has_more;
	response["text_array"] = Json::arrayValue;
	for (const auto& message : messages) {
		Json::Value item;
		item["message_id"] = Json::Int64(message.message_id);
		item["thread_id"] = Json::Int64(message.thread_id);
		item["fromuid"] = message.sender_id;
		item["touid"] = message.recv_id;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["msg_type"] = message.msg_type; //0 文本, 1 图片, 2 视频, 3 文件, 4 群系统消息
		item["time"] = Json::Int64(message.created_at); //UNIX 秒, 客户端按本地时区分层显示
		//自己发的消息带 read 表示对方是否已读；recalled 为真时只渲染一行提示，不显示正文
		item["read"] = message.is_read;
		item["recalled"] = message.recalled;
		item["mentions"] = message.mentions;
		//历史消息按"自己发的/别人发的"给默认状态；群聊不做逐条已读，自己发的都算已发送
		item["status"] = message.sender_id == uid ? 1 : 3;
		response["text_array"].append(item);
	}
	session->Send(JsonToCompact(response), ID_LOAD_CHAT_MSG_RSP);
}

void LogicSystem::PushChatEvent(int from_uid, int to_uid, long long thread_id,
	long long message_id, int event)
{
	if (from_uid <= 0 || to_uid <= 0 || thread_id <= 0)
		return;
	//投递交给独立线程：对端实例不可达时 RPC 会等到截止时间，
	//已读/撤回通知晚几百毫秒不影响正确性
	PostDeliveryTask([this, from_uid, to_uid, thread_id, message_id, event]() {
		PushChatEventNow(from_uid, to_uid, thread_id, message_id, event);
	});
}

void LogicSystem::PushChatEventNow(int from_uid, int to_uid, long long thread_id,
	long long message_id, int event)
{
	if (from_uid <= 0 || to_uid <= 0 || thread_id <= 0)
		return;

	std::string ip;
	if (!SessionRouter::Inst().Lookup(to_uid, ip))
		return;

	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	if (ip == self_name) {
		auto peer = UserMgr::GetInstance()->GetSession(to_uid);
		if (!peer)
			return;

		Json::Value notify;
		notify["error"] = ErrorCodes::Success;
		notify["thread_id"] = Json::Int64(thread_id);
		notify["message_id"] = Json::Int64(message_id);
		if (event == 1) {
			//已读：客户端按 uid 确认是哪位好友读的
			notify["uid"] = from_uid;
			peer->Send(JsonToCompact(notify), ID_NOTIFY_MSG_READ);
		}
		else {
			notify["fromuid"] = from_uid;
			peer->Send(JsonToCompact(notify), ID_NOTIFY_MSG_RECALLED);
		}
		return;
	}

	ChatEventReq req;
	req.set_fromuid(from_uid);
	req.set_touid(to_uid);
	req.set_thread_id(thread_id);
	req.set_message_id(message_id);
	req.set_event(event);
	ChatGrpcClient::GetInstance()->NotifyChatEvent(ip, req);
}

//上报已读位置：客户端打开会话或收到新消息时调用，重复上报不会产生多余通知
void LogicSystem::MarkMessagesRead(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const long long thread_id = root["thread_id"].asInt64();
	const long long message_id = root["message_id"].asInt64();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_MARK_READ_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (thread_id <= 0 || message_id <= 0) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	const int thread_type = ThreadTypeOf(thread_id);
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		//群聊只推进自己的已读游标，未读数与会话行"有人@我"由它推导；
		//群里不展示逐条已读，所以不向其他成员推送
		if (!MysqlDao::GetInstance()->MarkGroupRead(uid, thread_id, message_id)) {
			rtvalue["error"] = ErrorCodes::RPCFailed;
			return;
		}
		rtvalue["thread_id"] = Json::Int64(thread_id);
		rtvalue["message_id"] = Json::Int64(message_id);
		return;
	}

	bool changed = false;
	int peer_uid = 0;
	if (!MysqlDao::GetInstance()->MarkChatMessagesRead(uid, thread_id, message_id, changed, peer_uid)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
	rtvalue["thread_id"] = Json::Int64(thread_id);
	rtvalue["message_id"] = Json::Int64(message_id);

	//没有新消息被标记时不必通知对方，避免每次切会话都推一遍
	if (changed)
		PushChatEvent(uid, peer_uid, thread_id, message_id, 1);
}

//撤回消息：自己这一侧靠回包更新，对方那一侧靠通知
void LogicSystem::RecallMessage(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{
	const long long message_id = root["message_id"].asInt64();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(JsonToCompact(rtvalue), ID_RECALL_MSG_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (message_id <= 0) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	ChatMessageInfo message;
	const int code = MysqlDao::GetInstance()->RecallChatMessage(uid, message_id, message);
	if (code != ErrorCodes::Success) {
		rtvalue["error"] = code;
		return;
	}

	//thread_id 以库里为准，客户端传错也不会把通知发到别的会话
	rtvalue["thread_id"] = Json::Int64(message.thread_id);
	rtvalue["message_id"] = Json::Int64(message.message_id);
	rtvalue["thread_type"] = ThreadTypeOf(message.thread_id);
	if (rtvalue["thread_type"].asInt() == static_cast<int>(ThreadType::Group)) {
		//群内撤回：除自己外全部成员都换成提示行，离线成员登录拉历史时看到 recalled
		Json::Value payload;
		payload["error"] = ErrorCodes::Success;
		payload["thread_id"] = Json::Int64(message.thread_id);
		payload["thread_type"] = static_cast<int>(ThreadType::Group);
		payload["message_id"] = Json::Int64(message.message_id);
		payload["fromuid"] = uid;
		std::vector<int> members;
		if (MysqlDao::GetInstance()->GetGroupMemberIds(message.thread_id, members))
			BroadcastGroupEvent(message.thread_id, members, uid, payload, ID_NOTIFY_MSG_RECALLED);
		return;
	}
	PushChatEvent(uid, message.recv_id, message.thread_id, message.message_id, 2);
}

//删除好友：只改关系表，不推送、不通知。对方那边的好友列表要到下次登录才发现，
