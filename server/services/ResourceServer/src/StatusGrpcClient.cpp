#include "StatusGrpcClient.h"
#include "ConfigMgr.h"
#include <chrono>
#include <iostream>
#include "Log.h"

StatusGrpcClient::StatusGrpcClient()
{
	auto& cfg = ConfigMgr::Inst();
	const std::string host = cfg["StatusServer"]["Host"];
	const std::string port = cfg["StatusServer"]["Port"];
	if (host.empty() || port.empty()) {
		LOG_WARN << "[ResourceServer] 未配置 [StatusServer]，在线位置查询将不可用";
		return;
	}
	auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
	_stub = message::StatusService::NewStub(channel);
}

bool StatusGrpcClient::LookupRoute(int uid, std::string& instance)
{
	instance.clear();
	if (uid <= 0 || !_stub)
		return false;

	message::LookupRouteReq request;
	request.set_uid(uid);
	message::LookupRouteRsp response;

	grpc::ClientContext context;
	//查询只是推送的前置条件，失败就让调用方跳过实时通知，消息已经在库里
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(800));

	const grpc::Status status = _stub->LookupRoute(&context, request, &response);
	if (!status.ok() || response.error() != 0 || !response.online())
		return false;

	instance = response.instance();
	return !instance.empty();
}
