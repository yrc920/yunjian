#pragma once
// 状态服务客户端：登录租约、续租、释放、位置查询与实例注册
// 所有调用都设置截止时间，状态服务变慢时按失败处理，不阻塞连接线程
//

#include "const.h"
#include "Singleton.h"
#include <grpcpp/grpcpp.h>
#include "message.grpc.pb.h"
#include "message.pb.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class StatusConPool;

using grpc::Channel;

class StatusGrpcClient : public Singleton<StatusGrpcClient>
{
	friend class Singleton<StatusGrpcClient>;
public:
	~StatusGrpcClient();

	//登录：申请租约并拿到 epoch；被顶替的旧实例由状态服务异步通知
	message::AllocateSessionRsp AllocateSession(int uid, const std::string& token,
		const std::string& session_id, const std::string& instance);
	//批量续租，同时上报本实例负载
	message::RenewSessionsRsp RenewSessions(const std::string& instance,
		const std::vector<message::SessionLeaseRef>& leases, int conn_count, int session_count);
	//释放租约：连接关闭时调用，失败不影响本地清理
	message::ReleaseSessionRsp ReleaseSession(int uid, const std::string& session_id, long long epoch);
	//显式登出：按 sid + epoch 校验后吊销租约与令牌，旧连接无法吊销新会话
	message::LogoutSessionRsp LogoutSession(int uid, const std::string& session_id, long long epoch);
	//释放租约的异步版本：断连路径不能因为状态服务无响应而阻塞 IO 线程
	void ReleaseAsync(int uid, const std::string& session_id, long long epoch);

	message::LookupRouteRsp LookupRoute(int uid);
	message::BatchLookupRouteRsp BatchLookupRoute(const std::vector<int>& uids);

	message::RegisterInstanceRsp RegisterInstance(const std::string& name, const std::string& host,
		const std::string& rpc_host, int client_port, int rpc_port, int capacity);
	message::UnregisterInstanceRsp UnregisterInstance(const std::string& name);

	//排空释放队列并停止工作线程，进程退出前调用
	void Shutdown();

private:
	StatusGrpcClient();

	void ReleaseLoop();

	struct ReleaseTask {
		int uid = 0;
		std::string session_id;
		long long epoch = 0;
	};

	std::unique_ptr<StatusConPool> pool_;

	std::mutex _queue_mutex;
	std::condition_variable _queue_cv;
	std::deque<ReleaseTask> _release_tasks;
	std::atomic<bool> _stop{ false };
	std::thread _release_thread;
};

class StatusConPool
{
public:
	StatusConPool(size_t poolSize, const std::string& host, const std::string& port);
	//获取连接，最多等待 timeout_ms；连接池停止或超时返回空指针
	std::unique_ptr<message::StatusService::Stub> getConnection(int timeout_ms = 1000);
	void returnConnection(std::unique_ptr<message::StatusService::Stub> context);
	void Close();
	~StatusConPool();

private:
	std::atomic<bool> b_stop_;
	size_t poolSize_;
	std::string host_;
	std::string port_;
	std::queue<std::unique_ptr<message::StatusService::Stub>> connections_;
	std::mutex mutex_;
	std::condition_variable cond_;
};
