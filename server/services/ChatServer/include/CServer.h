#pragma once
// 服务器类, 负责监听端口, 接受连接, 管理会话
//

#include <boost/asio.hpp>
#include "CSession.h"
#include <memory>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>

using boost::asio::ip::tcp;

class CServer : public std::enable_shared_from_this<CServer>
{
public:
	//tls 为空时客户端连接为明文 TCP；非空则每条连接先握手
	CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<TlsContext> tls);
	~CServer();
	void ClearSession(const std::string&); //清除指定uuid的会话
    bool CheckValid(const std::string&); //检查指定uuid的会话是否存在
    std::shared_ptr<CSession> GetSession(const std::string&); //根据uuid获取session

    void on_timer(const boost::system::error_code& ec); //定时器回调函数, 用于清理无效会话
    void StartTimer(); //启动定时器, 定时清理无效会话
    void StopTimer(); //停止定时器, 停止清理无效会话
    void Stop(); //幂等停服：停止接入、定时器并清理所有会话

private:
	//处理接受连接的回调函数
	void HandleAccept(std::shared_ptr<CSession>, const boost::system::error_code& error);
	void StartAccept(); //开始接受连接
	bool RegisterSelf(); //向状态服务登记本实例，失败时每个定时周期重试
	void RenewLeases(const std::vector<std::shared_ptr<CSession>>& sessions,
		int conn_count, int session_count); //批量续租并处理被顶替的会话

	boost::asio::io_context& _io_context; //主线程的io_context, 负责accept
	short _port; //监听的端口
	tcp::acceptor _acceptor; //acceptor对象, 用于接受连接
	std::map<std::string, std::shared_ptr<CSession>> _sessions; //存储所有会话
	std::mutex _mutex; //保护_sessions的互斥锁
    boost::asio::steady_timer _timer; //定时器, 用于心跳检测、续租与实例上报
    int _stats_tick = 0; //每两个心跳周期输出一次运行指标
    std::atomic<bool> _stopped{false}; //标记服务器是否已停止, 用于控制定时器和会话清理的行为

    std::string _self_name; //本实例名，状态服务据此识别心跳
    std::string _advertise_host; //客户端可访问的地址
    std::string _advertise_rpc_host; //实例间 RPC 使用的主机名，容器部署时与客户端地址不同
    int _client_port = 0; //客户端连接的 TCP 端口
    int _rpc_port = 0; //实例间 gRPC 端口
    int _capacity = 1000; //期望承载的会话数
    bool _registered = false; //注册是否有效，续租返回 InstanceUnknown 时置回 false
    int _renew_failures = 0; //连续续租失败次数，只用于日志分级，不作为踢人依据
    bool _draining = false; //状态服务标记的下线状态
    std::shared_ptr<TlsContext> _tls; //空 = 明文接入
};

