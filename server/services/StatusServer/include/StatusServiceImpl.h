#pragma once
// 状态服务实现：登录选路、会话租约、在线位置查询与实例注册
//

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include <string>

using grpc::ServerContext;
using grpc::Status;
using message::StatusService;
using message::GetChatServerReq;
using message::GetChatServerRsp;
using message::AllocateSessionReq;
using message::AllocateSessionRsp;
using message::RenewSessionsReq;
using message::RenewSessionsRsp;
using message::ReleaseSessionReq;
using message::ReleaseSessionRsp;
using message::LogoutSessionReq;
using message::LogoutSessionRsp;
using message::LookupRouteReq;
using message::LookupRouteRsp;
using message::BatchLookupRouteReq;
using message::BatchLookupRouteRsp;
using message::RegisterInstanceReq;
using message::RegisterInstanceRsp;
using message::UnregisterInstanceReq;
using message::UnregisterInstanceRsp;
using message::DrainInstanceReq;
using message::DrainInstanceRsp;

class StatusServiceImpl final : public StatusService::Service
{
public:
	StatusServiceImpl();

	Status GetChatServer(ServerContext* context, const GetChatServerReq* request,
		GetChatServerRsp* reply) override;
	Status AllocateSession(ServerContext* context, const AllocateSessionReq* request,
		AllocateSessionRsp* reply) override;
	Status RenewSessions(ServerContext* context, const RenewSessionsReq* request,
		RenewSessionsRsp* reply) override;
	Status ReleaseSession(ServerContext* context, const ReleaseSessionReq* request,
		ReleaseSessionRsp* reply) override;
	Status LogoutSession(ServerContext* context, const LogoutSessionReq* request,
		LogoutSessionRsp* reply) override;
	Status LookupRoute(ServerContext* context, const LookupRouteReq* request,
		LookupRouteRsp* reply) override;
	Status BatchLookupRoute(ServerContext* context, const BatchLookupRouteReq* request,
		BatchLookupRouteRsp* reply) override;
	Status RegisterInstance(ServerContext* context, const RegisterInstanceReq* request,
		RegisterInstanceRsp* reply) override;
	Status UnregisterInstance(ServerContext* context, const UnregisterInstanceReq* request,
		UnregisterInstanceRsp* reply) override;
	Status DrainInstance(ServerContext* context, const DrainInstanceReq* request,
		DrainInstanceRsp* reply) override;

private:
	bool VerifyToken(int uid, const std::string& token); //比对 utoken_<uid>，只读
	bool IssueToken(int uid, std::string& token); //签发并写入带过期时间的令牌

	std::string _res_host; //资源服务器地址，随登录路由一起下发
	std::string _res_port;
};
