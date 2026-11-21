#include "ChatServiceImpl.h"
#include "JsonUtil.h"
#include "UserCache.h"
#include "UserMgr.h"
#include "CSession.h"
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include "RedisMgr.h" 
#include "MysqlDao.h"
#include "LogicSystem.h"
#include "Log.h"

ChatServiceImpl::ChatServiceImpl() {}

Status ChatServiceImpl::NotifyAddFriend(ServerContext* context,
	const AddFriendReq* request, AddFriendRsp* reply)
{
	auto touid = request->touid(); //获取被申请人id
	auto session = UserMgr::GetInstance()->GetSession(touid); //获取被申请人session

	Defer defer([request, reply]() {
		reply->set_error(ErrorCodes::Success); //默认成功
		reply->set_applyuid(request->applyuid()); //设置申请人id
		reply->set_touid(request->touid()); //设置被申请人id
		});

	//如果用户不在内存中则直接返回
	if (session == nullptr) {
		return Status::OK;
	}

	//在内存中则直接发送通知对方
	Json::Value rtvalue; //构造json数据
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["applyuid"] = request->applyuid();
	rtvalue["name"] = request->name();
	rtvalue["desc"] = request->desc();
	rtvalue["icon"] = request->icon();
	rtvalue["sex"] = request->sex();
	rtvalue["nick"] = request->nick();
	std::string return_str = JsonToCompact(rtvalue); //将json数据转换为字符串

	session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ); //发送通知对方
	return Status::OK;
}

Status ChatServiceImpl::NotifyAuthFriend(ServerContext* context,
	const AuthFriendReq* request, AuthFriendRsp* reply)
{
	//查找用户是否在本服务器
	auto touid = request->touid();
	auto fromuid = request->fromuid();
	auto session = UserMgr::GetInstance()->GetSession(touid);

	Defer defer([request, reply]() {
		reply->set_error(ErrorCodes::Success);
		reply->set_fromuid(request->fromuid());
		reply->set_touid(request->touid());
		reply->set_thread_id(request->thread_id());
		});

	//用户不在内存中则直接返回
	if (session == nullptr) {
		return Status::OK;
	}

	//在内存中则直接发送通知对方
	Json::Value  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = request->fromuid();
	rtvalue["touid"] = request->touid();
	rtvalue["thread_id"] = Json::Int64(request->thread_id());
	rtvalue["back"] = request->back(); //申请方当初给对方设的备注
	rtvalue["tags"] = Json::arrayValue;
	for (const auto& tag : request->tags())
		rtvalue["tags"].append(tag);
	//把proto里的欢迎消息还原成本实例客户端认识的JSON结构
	rtvalue["welcome_msgs"] = Json::arrayValue;
	for (const auto &msg : request->welcome_msgs()) {
		Json::Value item;
		item["msgid"] = msg.msgid();
		item["content"] = msg.msgcontent();
		item["message_id"] = Json::Int64(msg.message_id());
		item["thread_id"] = Json::Int64(request->thread_id());
		item["status"] = msg.status();
		item["fromuid"] = msg.fromuid();
		item["touid"] = msg.touid();
		rtvalue["welcome_msgs"].append(item);
	}

	UserInfo user_info;
	if (usercache::Get(fromuid, user_info)) {
		rtvalue["name"] = user_info.name;
		rtvalue["nick"] = user_info.nick;
		rtvalue["icon"] = user_info.icon;
		rtvalue["sex"] = user_info.sex;
	}
	else {
		rtvalue["error"] = ErrorCodes::UidInvalid;
	}

	std::string return_str = JsonToCompact(rtvalue);

	session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
	return Status::OK;
}

Status ChatServiceImpl::NotifyTextChatMsg(::grpc::ServerContext* context,
	const TextChatMsgReq* request, TextChatMsgRsp* reply)
{
	//查找用户是否在本服务器
	auto touid = request->touid();
	auto session = UserMgr::GetInstance()->GetSession(touid);
	reply->set_error(ErrorCodes::Success);
	reply->set_thread_id(request->thread_id());

	//目标不在本实例：返回 UidInvalid 让发送方失效路由缓存并重试一次，
	//不能一律回成功，否则用户换实例后这条消息会静默地只落库不推送
	if (session == nullptr) {
		reply->set_error(ErrorCodes::UidInvalid);
		return Status::OK;
	}

	//在内存中则直接发送通知对方
	Json::Value  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["fromuid"] = request->fromuid();
	rtvalue["touid"] = request->touid();
	rtvalue["thread_id"] = Json::Int64(request->thread_id());

	//将聊天数据组织为数组
	Json::Value text_array;
	for (auto& msg : request->textmsgs()) {
		Json::Value element;
		element["content"] = msg.msgcontent();
		element["msgid"] = msg.msgid();
		element["msg_type"] = msg.msg_type();
		element["message_id"] = Json::Int64(msg.message_id());
		element["thread_id"] = Json::Int64(request->thread_id());
		element["status"] = msg.status();
		element["time"] = Json::Int64(msg.time());
		text_array.append(element);
	}
	rtvalue["text_array"] = text_array;

	std::string return_str = JsonToCompact(rtvalue);

	session->Send(return_str, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
	return Status::OK;
}

Status ChatServiceImpl::KickSession(::grpc::ServerContext* context,
    const KickSessionReq* request, KickSessionRsp* reply)
{
    reply->set_error(ErrorCodes::Success);
    reply->set_closed(false);

    const int uid = request->uid();
    if (uid <= 0 || request->epoch() <= 0) {
        reply->set_error(ErrorCodes::Error_Json);
        return Status::OK;
    }

    //按 session_id 找旧连接：uid 映射可能已经被同实例的新登录覆盖，
    //按 uid 找会把踢人请求打到新连接上，表现为旧连接一直在线
    auto session = _p_server->GetSession(request->session_id());
    if (session == nullptr) {
        return Status::OK;
    }

    //防御：session_id 与 uid 必须对得上；本地 epoch 不小于通知里的 epoch 说明本地更新
    if (session->GetUserId() != uid || session->GetEpoch() >= request->epoch()) {
        return Status::OK;
    }

    //先发下线原因再延迟断开，保证客户端能收到"为什么被下线"
    session->NotifyOffline(uid, request->reason());
    session->CloseSoon(300);
    _p_server->ClearSession(session->GetSessionId());
    reply->set_closed(true);
    return Status::OK;
}

void ChatServiceImpl::RegisterServer(std::shared_ptr<CServer> pServer)
{
    _p_server = pServer; //注册服务器
}

Status ChatServiceImpl::NotifyImageUploaded(ServerContext* context,
    const NotifyImageUploadedReq* request, NotifyImageUploadedRsp* response)
{
    response->set_message_id(request->message_id());
    //群聊的 receiver 为 0：没有单一接收方，改按成员列表广播
    const bool group_resource = request->thread_type() == 2;
    if (request->message_id() <= 0 || request->sender() <= 0 || request->name().empty()
        || (!group_resource && request->receiver() <= 0)) {
        response->set_error(ErrorCodes::Error_Json);
        return Status::OK;
    }

    //资源类型、原始文件名与视频首帧都在落库时写好了，这里以库里的记录为准，
    //不依赖上传请求再带一遍，接收方拿到的通知就能直接渲染卡片
    ChatMessageInfo message;
    if (!MysqlDao::GetInstance()->GetChatMessage(request->message_id(), message)
        || message.sender_id != request->sender()) {
        response->set_error(ErrorCodes::RPCFailed);
        return Status::OK;
    }

    //聊天图片/视频/文件全部落盘后，把消息从“未上传”改成“已上传”
    const bool ok = MysqlDao::GetInstance()->UpdateChatMessageStatus(
        message.message_id, message.sender_id, 1);
    response->set_error(ok ? ErrorCodes::Success : ErrorCodes::RPCFailed);

    //接收方在本实例在线时立即推送；资源服务器已向状态服务查好目标实例。
    //群聊没有单一接收方：按成员列表广播，发送者自己已经看到消息，跳过
    if (ok) {
        const int thread_type = MysqlDao::GetInstance()->GetThreadTypeCached(message.thread_id);
        if (thread_type == static_cast<int>(ThreadType::Group)) {
            std::vector<int> members;
            if (MysqlDao::GetInstance()->GetGroupMemberIds(message.thread_id, members)) {
                Json::Value payload;
                payload["error"] = ErrorCodes::Success;
                payload["thread_id"] = Json::Int64(message.thread_id);
                payload["thread_type"] = thread_type;
                payload["fromuid"] = message.sender_id;
                payload["touid"] = 0;
                payload["msg_type"] = message.msg_type;
                payload["message_id"] = Json::Int64(message.message_id);
                payload["msgid"] = message.client_msg_id;
                payload["time"] = Json::Int64(message.created_at);
                payload["name"] = request->name();
                payload["total_size"] = Json::Int64(request->total_size());
                if (message.msg_type != 1)
                    payload["content"] = message.content;
                LogicSystem::GetInstance()->BroadcastGroupEvent(message.thread_id, members,
                    message.sender_id, payload,
                    message.msg_type == 1 ? ID_NOTIFY_IMG_CHAT_MSG_REQ
                                          : ID_NOTIFY_RESOURCE_CHAT_MSG_REQ);
            }
            return Status::OK;
        }

        auto session = UserMgr::GetInstance()->GetSession(message.recv_id);
        if (session) {
            if (message.msg_type == 1) {
                session->NotifyImageRecv(message.message_id, message.sender_id,
                    message.recv_id, message.thread_id, request->name(),
                    message.client_msg_id, request->total_size(), message.created_at);
            }
            else {
                //视频/文件只发通知，接收方点击卡片后才开始下载
                session->NotifyResourceRecv(message.message_id, message.sender_id,
                    message.recv_id, message.thread_id, request->name(),
                    message.client_msg_id, request->total_size(),
                    message.msg_type, message.content, message.created_at);
            }
        }
    }
    return Status::OK;
}

//资源服务器换完头像只知道 uid, 不知道好友列表: 交给逻辑层查好友并按实例分发。
//这条路径与客户端改资料(3016)汇合在同一个推送方法里, 头像和昵称走同一套通知。
Status ChatServiceImpl::BroadcastProfile(ServerContext* context,
    const BroadcastProfileReq* request, BroadcastProfileRsp* response)
{
    response->set_error(ErrorCodes::Success);
    if (request->uid() <= 0) {
        response->set_error(ErrorCodes::UidInvalid);
        return Status::OK;
    }

    LogicSystem::GetInstance()->NotifyFriendsProfileChanged(request->uid());
    return Status::OK;
}

//跨实例资料变更: 只推给 touids 里在本实例在线的那些人, 离线的跳过
Status ChatServiceImpl::NotifyProfileChanged(ServerContext* context,
    const ProfileChangedReq* request, ProfileChangedRsp* response)
{
    response->set_error(ErrorCodes::Success);
    if (request->fromuid() <= 0 || request->touids_size() == 0) {
        response->set_error(ErrorCodes::Error_Json);
        return Status::OK;
    }

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["uid"] = request->fromuid();
    rtvalue["nick"] = request->nick();
    rtvalue["desc"] = request->desc();
    rtvalue["icon"] = request->icon();
    rtvalue["sex"] = request->sex();
    const std::string notify_text = JsonToCompact(rtvalue);

    for (int i = 0; i < request->touids_size(); ++i) {
        auto session = UserMgr::GetInstance()->GetSession(request->touids(i));
        if (session)
            session->Send(notify_text, ID_NOTIFY_PROFILE_CHANGED);
    }
    return Status::OK;
}

//跨实例会话事件：已读把单勾改双勾，撤回把气泡换成提示。
//对方不在本实例时直接返回成功——状态已在库里，下次拉历史能看到
Status ChatServiceImpl::NotifyChatEvent(ServerContext* context,
    const ChatEventReq* request, ChatEventRsp* response)
{
    response->set_error(ErrorCodes::Success);
    if (request->fromuid() <= 0 || request->touid() <= 0 || request->thread_id() <= 0) {
        response->set_error(ErrorCodes::Error_Json);
        return Status::OK;
    }

    auto session = UserMgr::GetInstance()->GetSession(request->touid());
    if (!session)
        return Status::OK;

    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["thread_id"] = Json::Int64(request->thread_id());
    rtvalue["message_id"] = Json::Int64(request->message_id());
    if (request->event() == 1) {
        rtvalue["uid"] = request->fromuid(); //读取者
        session->Send(JsonToCompact(rtvalue), ID_NOTIFY_MSG_READ);
    }
    else if (request->event() == 2) {
        rtvalue["fromuid"] = request->fromuid(); //撤回者
        session->Send(JsonToCompact(rtvalue), ID_NOTIFY_MSG_RECALLED);
    }
    return Status::OK;
}

//跨实例群事件：payload 已经由发送方实例组装成客户端可直接解析的 JSON，
//这里只负责按 uid 找到本实例的会话并原样投递；离线成员跳过，入库内容不会丢
Status ChatServiceImpl::NotifyGroupEvent(ServerContext* context,
    const GroupEventReq* request, GroupEventRsp* response)
{
    response->set_error(ErrorCodes::Success);
    if (request->thread_id() <= 0 || request->notify_id() <= 0 || request->payload().empty()) {
        response->set_error(ErrorCodes::Error_Json);
        return Status::OK;
    }

    for (int i = 0; i < request->touids_size(); ++i) {
        auto session = UserMgr::GetInstance()->GetSession(request->touids(i));
        if (session)
            session->Send(request->payload(), static_cast<short>(request->notify_id()));
    }
    return Status::OK;
}
