#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include <chrono>
#include <iostream>

ChatGrpcClient::ChatGrpcClient()
{
	auto& cfg = ConfigMgr::Inst();
	const std::string host = cfg["ChatServer"]["Host"];
	const std::string port = cfg["ChatServer"]["Port"];
	if (host.empty() || port.empty()) {
		//没配置时保留空stub，调用方会退回到直接更新数据库
		std::cerr << "[ResourceServer] [ChatServer] 未配置，图片状态回写将直接走 MySQL" << std::endl;
		return;
	}

	auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
	_stub = message::ChatService::NewStub(channel);
}

bool ChatGrpcClient::NotifyImageUploaded(long long message_id, int sender, long long thread_id,
	const std::string& name)
{
	if (!_stub)
		return false;

	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

	message::NotifyImageUploadedReq request;
	request.set_message_id(message_id);
	request.set_sender(sender);
	request.set_thread_id(thread_id);
	request.set_name(name);

	message::NotifyImageUploadedRsp response;
	const grpc::Status status = _stub->NotifyImageUploaded(&context, request, &response);
	if (!status.ok()) {
		std::cerr << "NotifyImageUploaded failed: " << status.error_message() << std::endl;
		return false;
	}
	return response.error() == 0;
}
