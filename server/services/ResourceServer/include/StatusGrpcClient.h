#pragma once
// 资源服务器到状态服务的最小客户端：只做在线位置查询
// 资源上传完成后需要知道接收方在哪台 ChatServer 上，位置权威在状态服务
//

#include "const.h"
#include "Singleton.h"
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include <memory>
#include <mutex>
#include <string>

class StatusGrpcClient : public Singleton<StatusGrpcClient>
{
	friend class Singleton<StatusGrpcClient>;
public:
	// 在线时返回实例名；离线或状态服务不可用返回 false
	bool LookupRoute(int uid, std::string& instance);

private:
	StatusGrpcClient();

	std::unique_ptr<message::StatusService::Stub> _stub;
};
