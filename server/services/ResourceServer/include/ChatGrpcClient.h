#pragma once
// 资源服务器 -> ChatServer 的 gRPC 通知客户端
//
// 聊天图片全部落盘后，把 message_id 交给 ChatServer 更新 chat_message.status。
// 只连一台配置好的 ChatServer；状态更新不依赖会话位置，数据库是共享的。

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include "message.grpc.pb.h"
#include "Singleton.h"

class ChatGrpcClient : public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	~ChatGrpcClient() = default;

	//返回 true 表示 ChatServer 已把状态改成已上传；失败时调用方回退到直接写库
	bool NotifyImageUploaded(long long message_id, int sender, long long thread_id,
		const std::string& name);

private:
	ChatGrpcClient();

	std::shared_ptr<message::ChatService::Stub> _stub; //未配置[ChatServer]时为空
};
