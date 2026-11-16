#pragma once
// Grpc服务器实现类，负责处理客户端请求
//

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "message.pb.h"
#include <mutex>
#include "data.h"
#include "CServer.h"
#include <memory>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using message::AddFriendReq;
using message::AddFriendRsp;
using message::AuthFriendReq;
using message::AuthFriendRsp;
using message::ChatService;
using message::TextChatMsgReq;
using message::TextChatMsgRsp;
using message::TextChatData;
using message::KickSessionReq;
using message::KickSessionRsp;
using message::NotifyImageUploadedReq;
using message::NotifyImageUploadedRsp;
using message::BroadcastProfileReq;
using message::BroadcastProfileRsp;
using message::ProfileChangedReq;
using message::ProfileChangedRsp;
using message::ChatEventReq;
using message::ChatEventRsp;
using message::GroupEventReq;
using message::GroupEventRsp;

class ChatServiceImpl final : public ChatService::Service
{
public:
	ChatServiceImpl();
	Status NotifyAddFriend(ServerContext* context,
		const AddFriendReq* request, AddFriendRsp* reply) override; //处理添加好友请求
	Status NotifyAuthFriend(ServerContext* context,
		const AuthFriendReq* request, AuthFriendRsp* response) override; //处理好友验证请求
	Status NotifyTextChatMsg(ServerContext* context,
		const TextChatMsgReq* request, TextChatMsgRsp* response) override; //处理文本聊天消息请求
    //租约被更高 epoch 顶替时，状态服务通知本实例关闭旧连接
    Status KickSession(::grpc::ServerContext* context,
        const KickSessionReq* request, KickSessionRsp* response) override;
    //资源服务器在聊天图片完整落盘后通知这里回写消息状态
    Status NotifyImageUploaded(ServerContext* context,
        const NotifyImageUploadedReq* request, NotifyImageUploadedRsp* response) override;
    //资源服务器换完头像后：查该用户的好友并分发资料变更
    Status BroadcastProfile(ServerContext* context,
        const BroadcastProfileReq* request, BroadcastProfileRsp* response) override;
    //跨实例资料变更：把新资料推给本实例上的这批好友
    Status NotifyProfileChanged(ServerContext* context,
        const ProfileChangedReq* request, ProfileChangedRsp* response) override;
    //跨实例会话事件：已读或撤回，推给本实例上的接收者
    Status NotifyChatEvent(ServerContext* context,
        const ChatEventReq* request, ChatEventRsp* response) override;
    //跨实例群事件：按 touids 把同一份通知投递给本实例上的群成员
    Status NotifyGroupEvent(ServerContext* context,
        const GroupEventReq* request, GroupEventRsp* response) override;

    bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo); //获取用户基本信息
    void RegisterServer(std::shared_ptr<CServer> pServer); //注册CServer对象，方便在处理请求时访问服务器相关功能

private:
    std::shared_ptr<CServer> _p_server; //CServer对象的智能指针，用于在处理请求时访问服务器相关功能
};

