#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include "StatusGrpcClient.h"
#include "const.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <sstream>

namespace {
std::string trim(std::string value)
{
	auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}
}

ChatGrpcClient::ChatGrpcClient()
{
	auto& cfg = ConfigMgr::Inst();
	const std::string servers = cfg["ChatServers"]["Servers"];
	std::stringstream stream(servers);
	std::string name;
	while (std::getline(stream, name, ',')) {
		name = trim(name);
		if (name.empty())
			continue;
		const std::string host = cfg[name]["Host"];
		const std::string port = cfg[name]["Port"];
		if (host.empty() || port.empty()) {
			std::cerr << "[ResourceServer] ChatServer 实例 " << name << " 缺少 Host/Port，已跳过" << std::endl;
			continue;
		}
		auto channel = grpc::CreateChannel(host + ":" + port, grpc::InsecureChannelCredentials());
		_stubs[name] = message::ChatService::NewStub(channel);
	}
	if (_stubs.empty())
		std::cerr << "[ResourceServer] [ChatServers] 未配置，图片完成通知将直接写 MySQL" << std::endl;
}

bool ChatGrpcClient::NotifyImageReady(long long message_id, int sender, int receiver,
	long long thread_id, const std::string& name, const std::string& client_msg_id,
	long long total_size, int thread_type)
{
	if (_stubs.empty())
		return false;

	//单聊看接收方是否在线；群聊没有单一接收方，改找发送者所在实例，
	//由它查成员列表并按实例广播，离线成员靠登录同步
	const bool group = thread_type == 2;
	const int target_uid = group ? sender : receiver;
	std::string server_name;
	if (!StatusGrpcClient::GetInstance()->LookupRoute(target_uid, server_name))
		return false;

	auto iter = _stubs.find(server_name);
	if (iter == _stubs.end()) {
		std::cerr << "no chatserver config for instance: " << server_name << std::endl;
		return false;
	}

	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

	message::NotifyImageUploadedReq request;
	request.set_message_id(message_id);
	request.set_sender(sender);
	request.set_receiver(receiver);
	request.set_thread_id(thread_id);
	request.set_name(name);
	request.set_client_msg_id(client_msg_id);
	request.set_total_size(total_size);
	request.set_thread_type(thread_type);

	message::NotifyImageUploadedRsp response;
	const grpc::Status status = iter->second->NotifyImageUploaded(&context, request, &response);
	if (!status.ok()) {
		std::cerr << "NotifyImageUploaded failed: " << status.error_message() << std::endl;
		return false;
	}
	return response.error() == 0;
}

//头像变更广播: 只带 uid, 好友列表与分发都在 ChatServer 侧完成
bool ChatGrpcClient::BroadcastProfileChanged(int uid)
{
	if (_stubs.empty() || uid <= 0)
		return false;

	std::string server_name;
	if (!StatusGrpcClient::GetInstance()->LookupRoute(uid, server_name))
		return false;

	auto iter = _stubs.find(server_name);
	if (iter == _stubs.end()) {
		std::cerr << "no chatserver config for instance: " << server_name << std::endl;
		return false;
	}

	grpc::ClientContext context;
	context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));

	message::BroadcastProfileReq request;
	request.set_uid(uid);
	message::BroadcastProfileRsp response;
	const grpc::Status status = iter->second->BroadcastProfile(&context, request, &response);
	if (!status.ok()) {
		std::cerr << "BroadcastProfile failed: " << status.error_message() << std::endl;
		return false;
	}
	return response.error() == 0;
}
