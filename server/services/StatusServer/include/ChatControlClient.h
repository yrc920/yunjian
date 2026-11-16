#pragma once
// 状态服务到聊天实例的控制链路：只用于通知旧连接下线
// 与消息转发无关，调用失败不影响租约状态，续租会在下个周期兜底
//

#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ChatControlClient
{
public:
	static ChatControlClient& Inst();

	// 异步投递踢人任务：按旧连接的 session_id 精确定位，失败不影响新租约
	void KickAsync(const std::string& host, int port, int uid, long long epoch,
		const std::string& session_id, int reason);
	// 等待已投递的任务处理完再退出
	void Shutdown();

private:
	ChatControlClient();
	~ChatControlClient();
	ChatControlClient(const ChatControlClient&) = delete;
	ChatControlClient& operator=(const ChatControlClient&) = delete;

	bool Kick(const std::string& host, int port, int uid, long long epoch,
		const std::string& session_id, int reason);
	std::shared_ptr<message::ChatService::Stub> StubFor(const std::string& address);
	void WorkerLoop();

	struct Task {
		std::string host;
		int port = 0;
		int uid = 0;
		long long epoch = 0;
		std::string session_id;
		int reason = 0;
	};

	std::mutex _stub_mutex;
	std::map<std::string, std::shared_ptr<message::ChatService::Stub>> _stubs;

	std::mutex _queue_mutex;
	std::condition_variable _queue_cv;
	std::deque<Task> _tasks;
	std::atomic<bool> _stop{ false };
	std::thread _worker;
};
