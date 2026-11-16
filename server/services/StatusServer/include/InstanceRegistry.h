#pragma once
// 聊天实例注册表：谁在线、地址是什么、当前承载多少会话
// 实例启动时注册、每 15 秒心跳续期，超过 45 秒没有心跳即从可选列表剔除
//

#include "const.h"
#include <string>
#include <vector>

struct InstanceInfo {
	std::string name;
	std::string host; //客户端可访问的地址
	std::string rpc_host; //实例间 RPC 使用的主机名
	int client_port = 0; //客户端连接的 TCP 端口
	int rpc_port = 0; //实例间 gRPC 端口
	int capacity = 0; //期望承载的会话数，用于加权调度
	int conns = 0; //未过期 TCP 连接数，含未登录
	int sessions = 0; //已认证会话数
	bool draining = false; //运维下线中，不再分配新登录
	long long started_at = 0;
};

class InstanceRegistry
{
public:
	static InstanceRegistry& Inst();

	// 实例名是否在配置白名单内；白名单为空时放行并在启动日志里提示
	bool Allowed(const std::string& name) const;

	bool Register(const std::string& name, const std::string& host, const std::string& rpc_host,
		int client_port, int rpc_port, int capacity, int& heartbeat_seconds, bool& draining);
	bool Unregister(const std::string& name);
	bool Heartbeat(const std::string& name, int conns, int sessions, bool& draining);
	bool Drain(const std::string& name, int grace_seconds);

	bool Get(const std::string& name, InstanceInfo& info);
	std::vector<InstanceInfo> List();

	// 选路：存活、未下线、地址完整，取 sessions/capacity 最小的一个
	bool Pick(InstanceInfo& info);

private:
	InstanceRegistry();

	std::vector<std::string> _allowed;
};
