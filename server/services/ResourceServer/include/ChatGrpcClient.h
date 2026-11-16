#pragma once
// 资源服务器 -> ChatServer 的 gRPC 通知客户端
//
// 聊天图片全部落盘后，先向 StatusServer 查询接收方所在的 ChatServer，
// 再通知它更新消息状态并把下载信息推给接收方；接收方离线时调用方直接写库兜底。
// 群聊没有单一接收方，改查发送者所在实例，由它按成员广播。

#include <grpcpp/grpcpp.h>
#include <memory>
#include <string>
#include <unordered_map>
#include "message.grpc.pb.h"
#include "Singleton.h"

class ChatGrpcClient : public Singleton<ChatGrpcClient>
{
	friend class Singleton<ChatGrpcClient>;
public:
	~ChatGrpcClient() = default;

	//返回 true 表示目标 ChatServer 已处理；false 时调用方回退到直接写库
	bool NotifyImageReady(long long message_id, int sender, int receiver, long long thread_id,
		const std::string& name, const std::string& client_msg_id, long long total_size,
		int thread_type);
	//头像换了之后，让该用户所在 ChatServer 把新资料推给他的在线好友
	bool BroadcastProfileChanged(int uid);

private:
	ChatGrpcClient();

	//实例名 -> stub；实例名与 [ChatServers] Servers 及 ChatServer 的 SelfServer.Name 对齐
	std::unordered_map<std::string, std::unique_ptr<message::ChatService::Stub>> _stubs;
};
