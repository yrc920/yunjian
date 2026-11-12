#include "ChatGrpcClient.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
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
	long long total_size)
{
	if (_stubs.empty())
		return false;

	//接收方离线时 Redis 没有位置键，不必调用任何实例；状态由调用方直接写库
	std::string server_name;
	if (!RedisMgr::GetInstance()->Get(USERIPPREFIX + std::to_string(receiver), server_name))
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

	message::NotifyImageUploadedRsp response;
	const grpc::Status status = iter->second->NotifyImageUploaded(&context, request, &response);
	if (!status.ok()) {
		std::cerr << "NotifyImageUploaded failed: " << status.error_message() << std::endl;
		return false;
	}
	return response.error() == 0;
}
