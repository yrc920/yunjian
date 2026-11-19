#pragma once
// 资源服务器会话：处理 2+4 字节协议、发送队列与连接清理
//

#include <boost/asio.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <queue>
#include <atomic>
#include <memory>
#include "const.h"
#include "MsgNode.h"
#include "TlsSocket.h"

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
class CServer;

class CSession : public std::enable_shared_from_this<CSession>
{
public:
	//tls 为空时按明文 TCP 服务；非空则连接先完成 TLS 握手再读第一帧
	CSession(boost::asio::io_context& io_context, CServer* server, std::shared_ptr<TlsContext> tls);
	~CSession();

	tcp::socket& GetSocket(); //返回socket引用，供acceptor绑定
	std::string& GetSessionId(); //返回会话UUID，逻辑线程按它分派
	void Start(); //投递到IO线程，开始读第一帧
	void Send(std::string msg, std::uint16_t msgid); //线程安全发送，内部排队到IO线程
	void Close(); //标记关闭并投递socket.close
	bool IsClosed() const { return _b_close.load(); } //是否已请求关闭

	//上传/下载首个请求通过token校验后把uid绑到连接，后续分片不再重复查Redis
	bool BindUser(int uid); //同一连接只能绑定一个uid
	int GetUserId() const { return _user_uid.load(); } //0表示尚未认证

	void AsyncReadBody(std::size_t length); //读满指定长度的包体
	void AsyncReadHead(); //读6字节头并解析ID/长度
	void DealExceptionSession(); //从CServer会话表移除自己，只执行一次

private:
	void SendOnIO(std::string msg, std::uint16_t msgid); //只在socket所属IO线程执行
	//循环补齐到指定长度，处理TCP拆包
	void asyncReadFull(std::size_t max_length,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	//递归读取剩余字节
	void asyncReadLen(std::size_t read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	//异步写完成：继续队首或清理失败队列
	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);

	TlsSocket _socket; //明文或 TLS 的统一封装，与CSession同属一个IO线程
	std::string _session_id; //逻辑线程按它做哈希分派
	std::atomic<int> _user_uid{0}; //0表示尚未通过token校验，非0后不再允许换人
	char _data[MAX_MESSAGE_LEN]; //一次只处理一帧，头部和包体都复用这块缓冲
	CServer* _server; //会话表由CServer持有，ClearSession回调用
	std::atomic<bool> _b_close{false}; //关闭意图，避免重复投递close
	std::queue<std::shared_ptr<SendNode>> _send_que; //只在IO线程访问
	std::atomic<std::size_t> _pending_sends{0}; //已入队+已投递，用于背压
	std::shared_ptr<RecvNode> _recv_msg_node; //当前包体，读完交给逻辑线程
	std::shared_ptr<MsgNode> _recv_head_node; //6字节头部缓冲，循环复用
	std::atomic<bool> _cleanup_started{false}; //保证只从会话表删除一次
};

class LogicNode {
	friend class LogicSystem;
	friend class LogicWorker;
public:
	LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode); //把一帧和它的会话一起入队

private:
	std::shared_ptr<CSession> _session; //持有会话，保证处理期间不被销毁
	std::shared_ptr<RecvNode> _recvnode; //完整包体与消息ID
};
