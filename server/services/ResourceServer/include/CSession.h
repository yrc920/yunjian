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

namespace net = boost::asio;
using tcp = boost::asio::ip::tcp;
class CServer;

class CSession : public std::enable_shared_from_this<CSession>
{
public:
	CSession(boost::asio::io_context& io_context, CServer* server);
	~CSession();

	tcp::socket& GetSocket();
	std::string& GetSessionId();
	void Start();
	void Send(std::string msg, std::uint16_t msgid);
	void Close();
	bool IsClosed() const { return _b_close.load(); }

	//上传/下载首个请求通过token校验后把uid绑到连接，后续分片不再重复查Redis
	bool BindUser(int uid);
	int GetUserId() const { return _user_uid.load(); }

	void AsyncReadBody(std::size_t length);
	void AsyncReadHead();
	void DealExceptionSession();

private:
	void SendOnIO(std::string msg, std::uint16_t msgid);
	void asyncReadFull(std::size_t max_length,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	void asyncReadLen(std::size_t read_len, std::size_t total_len,
		std::function<void(const boost::system::error_code&, std::size_t)> handler);
	void HandleWrite(const boost::system::error_code& error, std::shared_ptr<CSession> shared_self);

	tcp::socket _socket;
	std::string _session_id;
	std::atomic<int> _user_uid{0};
	char _data[MAX_MESSAGE_LEN];
	CServer* _server;
	std::atomic<bool> _b_close{false};
	std::queue<std::shared_ptr<SendNode>> _send_que;
	std::atomic<std::size_t> _pending_sends{0};
	std::shared_ptr<RecvNode> _recv_msg_node;
	std::shared_ptr<MsgNode> _recv_head_node;
	std::atomic<bool> _cleanup_started{false};
};

class LogicNode {
	friend class LogicSystem;
	friend class LogicWorker;
public:
	LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode);

private:
	std::shared_ptr<CSession> _session;
	std::shared_ptr<RecvNode> _recvnode;
};
