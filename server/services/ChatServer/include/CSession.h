#pragma once
// 会话类, 代表一个客户端连接, 负责与客户端通信
//

#include <boost/asio.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <queue>
#include <atomic>
#include <chrono>
#include <mutex>
#include <memory>
#include <cstddef>
#include <cstdint>
#include "const.h"
#include "MsgNode.h"

namespace net = boost::asio;            //from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       //from <boost/asio/ip/tcp.hpp>
class CServer;
class LogicSystem;

class CSession : public std::enable_shared_from_this<CSession>
{
public:
	CSession(boost::asio::io_context& io_context, CServer* server);
	~CSession();
	tcp::socket& GetSocket(); //返回socket对象的引用，以便服务器可以使用它进行通信
	std::string& GetSessionId(); //返回会话ID的引用，以便服务器可以使用它来标识会话
	void SetUserId(int uid); //设置用户ID
	int GetUserId(); //获取用户ID
	void SetEpoch(long long epoch); //登录租约的围栏令牌
	long long GetEpoch() const;

	void Start(); //启动会话, 开始处理连接
	void Send(std::string msg, std::uint16_t msgid); //发送消息
	void Send(const char* msg, std::size_t max_length, std::uint16_t msgid); //发送指定长度的消息
	void Close(); //关闭会话
	void CloseSoon(int delay_ms); //延迟关闭：先让下线通知写出去，再断开 socket
    bool IsClosed() const { return _b_close.load(); } //查询是否已请求关闭，不代表底层socket已经完成关闭
    //返回持有会话锁的RAII对象，调用方保留该对象期间，登录登记与断连清理不能同时进行
    std::unique_lock<std::mutex> LockLifecycle() { return std::unique_lock<std::mutex>(_lifecycle_mutex); }
	std::shared_ptr<CSession> SharedSelf(); //返回一个指向当前对象的shared_ptr
	void AsyncReadBody(int length); //异步读取消息体
	void AsyncReadHead(int total_len); //异步读取消息头
    //通知用户离线：reason 1 同账号异地登录 / 2 登录过期 / 3 服务端下线
    void NotifyOffline(int uid, int reason);
    //资源已上传完成：把消息 ID、会话与资源名推给接收方，接收方据此下载
    void NotifyImageRecv(long long message_id, int from_uid, int to_uid, long long thread_id,
        const std::string& name, const std::string& client_msg_id, long long total_size,
        long long created_at);
    //视频/文件已上传：带上类型与 content 元数据，接收方只显示卡片，点击后才下载
    void NotifyResourceRecv(long long message_id, int from_uid, int to_uid, long long thread_id,
        const std::string& name, const std::string& client_msg_id, long long total_size,
        int msg_type, const std::string& content, long long created_at);
    bool IsHeartbeatExpired(std::time_t& now); //判断心跳是否过期
    void UpdateHeartbeat(); //更新心跳
    //每连接消息令牌桶：防止单个客户端刷屏把逻辑队列打满
    bool AllowMessage();
    void DealExceptionSession(); //处理异常连接

private:
    void SendOnIO(std::string msg, std::uint16_t msgid); //只在 socket 所属 IO 线程中执行
	//读取完整长度
	void asyncReadFull(std::size_t maxLength,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	//读取指定字节数
	void asyncReadLen(std::size_t  read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	//处理写操作完成后的回调函数
	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);

	tcp::socket _socket; //套接字
	std::string _session_id; //连接的 UUID；与登录 token 分开使用
    std::atomic<int> _user_uid; //当前绑定的用户ID，0表示未认证，允许逻辑线程和清理线程读取
    std::atomic<long long> _epoch{0}; //登录租约的 epoch，清理时用它判断租约是否还属于本连接
	char _data[MAX_LENGTH]; //接收缓冲区
	CServer* _server; //指向服务器对象的指针
	std::atomic<bool> _b_close; //会话是否关闭
	std::queue<std::shared_ptr<SendNode> > _send_que; //仅由所属IO线程访问，队首是正在异步发送的消息
    std::atomic<std::size_t> _pending_sends{0}; //包含尚未进入 IO 线程的发送任务
	std::shared_ptr<RecvNode> _recv_msg_node; //收到的消息结构
	std::shared_ptr<MsgNode> _recv_head_node; //收到的头部结构
    std::atomic<time_t> _last_heartbeat; //最近收到完整消息的时间，供服务器定时器判断连接是否超时
    std::mutex _lifecycle_mutex; //登录登记与断连清理互斥
    std::atomic<bool> _cleanup_started{false}; //断连清理只执行一次
    //消息令牌桶状态：只在逻辑处理线程访问，用互斥锁把边界固定下来，
    //将来业务线程按会话分片时也不需要再改这里
    std::mutex _bucket_mutex;
    std::chrono::steady_clock::time_point _bucket_time{std::chrono::steady_clock::now()};
    double _bucket_tokens{50.0};
};

class LogicNode {
	friend class LogicSystem;
public:
	LogicNode(std::shared_ptr<CSession>, std::shared_ptr<RecvNode>);

private:
	std::shared_ptr<CSession> _session; //会话对象的指针
	std::shared_ptr<RecvNode> _recvnode; //收到的消息结构
};
