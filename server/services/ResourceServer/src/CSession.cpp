#include "CSession.h"
#include "CServer.h"
#include "LogicSystem.h"
#include <iostream>
#include <cstring>

CSession::CSession(boost::asio::io_context& io_context, CServer* server)
	: _socket(io_context), _server(server)
{
	boost::uuids::uuid uuid = boost::uuids::random_generator()();
	_session_id = boost::uuids::to_string(uuid);
	_recv_head_node = std::make_shared<MsgNode>(HEAD_TOTAL_LEN);
}

CSession::~CSession() = default;

tcp::socket& CSession::GetSocket()
{
	return _socket;
}

std::string& CSession::GetSessionId()
{
	return _session_id;
}

bool CSession::BindUser(int uid)
{
	if (uid <= 0)
		return false;

	int expected = 0;
	if (_user_uid.compare_exchange_strong(expected, uid))
		return true;
	return expected == uid;
}

void CSession::Start()
{
	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self]() {
		if (!self->IsClosed())
			self->AsyncReadHead();
	});
}

void CSession::Close()
{
	//读失败、写失败和停服都可能关闭同一连接，exchange保证只投递一次
	if (_b_close.exchange(true))
		return;

	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self]() {
		boost::system::error_code ec;
		self->_socket.close(ec);
	});
}

void CSession::Send(std::string msg, std::uint16_t msgid)
{
	if (IsClosed())
		return;

	if (msg.empty() || msg.size() > MAX_MESSAGE_LEN) {
		Close();
		DealExceptionSession();
		return;
	}

	//同时统计已入队和等待进入IO线程的发送任务，避免高并发下载时在io_context中积压
	if (_pending_sends.fetch_add(1) >= MAX_SENDQUE) {
		_pending_sends.fetch_sub(1);
		Close();
		DealExceptionSession();
		return;
	}

	auto self = shared_from_this();
	boost::asio::post(_socket.get_executor(), [self, msg = std::move(msg), msgid]() mutable {
		self->SendOnIO(std::move(msg), msgid);
	});
}

void CSession::SendOnIO(std::string msg, std::uint16_t msgid)
{
	if (IsClosed()) {
		_pending_sends.fetch_sub(1);
		return;
	}

	const bool writing = !_send_que.empty();
	_send_que.push(std::make_shared<SendNode>(msg.data(), msg.size(), msgid));
	if (writing)
		return;

	auto& node = _send_que.front();
	boost::asio::async_write(_socket, boost::asio::buffer(node->_data, node->_total_len),
		std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_from_this()));
}

void CSession::HandleWrite(const boost::system::error_code& error,
	std::shared_ptr<CSession> shared_self)
{
	if (error) {
		std::cout << "handle write failed, error is " << error.message() << std::endl;
		Close();
		DealExceptionSession();
		//异步写完成前不能清空队首缓冲区，此时回调已经返回，可以释放队列
		std::queue<std::shared_ptr<SendNode>> empty;
		_pending_sends.fetch_sub(_send_que.size());
		_send_que.swap(empty);
		return;
	}

	_send_que.pop();
	_pending_sends.fetch_sub(1);

	if (!_send_que.empty() && !IsClosed()) {
		auto& node = _send_que.front();
		boost::asio::async_write(_socket, boost::asio::buffer(node->_data, node->_total_len),
			std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_self));
	}
}

void CSession::AsyncReadHead()
{
	auto self = shared_from_this();
	asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code& ec,
		std::size_t bytes_transferred) {
		try {
			if (ec) {
				Close();
				DealExceptionSession();
				return;
			}

			if (bytes_transferred < HEAD_TOTAL_LEN) {
				Close();
				DealExceptionSession();
				return;
			}

			if (!_server->CheckValid(_session_id)) {
				Close();
				return;
			}

			_recv_head_node->Clear();
			memcpy(_recv_head_node->_data, _data, bytes_transferred);

			std::uint16_t msg_id = 0;
			memcpy(&msg_id, _recv_head_node->_data, HEAD_ID_LEN);
			msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id);

			std::uint32_t msg_len = 0;
			memcpy(&msg_len, _recv_head_node->_data + HEAD_ID_LEN, HEAD_DATA_LEN);
			msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len);

			//先拒绝空包和超过缓冲区的长度，再分配RecvNode，避免异常长度撑爆内存
			if (msg_id == 0 || msg_len == 0 || msg_len > MAX_MESSAGE_LEN) {
				std::cout << "invalid header, msg_id is " << msg_id
					<< ", msg_len is " << msg_len << std::endl;
				Close();
				DealExceptionSession();
				return;
			}

			_recv_msg_node = std::make_shared<RecvNode>(msg_len, msg_id);
			AsyncReadBody(msg_len);
		}
		catch (std::exception& e) {
			std::cerr << "Exception in AsyncReadHead: " << e.what() << std::endl;
		}
	});
}

void CSession::AsyncReadBody(std::size_t total_len)
{
	auto self = shared_from_this();
	asyncReadFull(total_len, [self, this, total_len](
		const boost::system::error_code& ec, std::size_t bytes_transferred) {
		try {
			if (ec) {
				Close();
				DealExceptionSession();
				return;
			}

			if (bytes_transferred < total_len) {
				Close();
				DealExceptionSession();
				return;
			}

			if (!_server->CheckValid(_session_id)) {
				Close();
				return;
			}

			memcpy(_recv_msg_node->_data, _data, bytes_transferred);
			_recv_msg_node->_cur_len = bytes_transferred;
			_recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';

			//按会话分派逻辑线程，分片顺序由客户端等待上一片回包保证
			LogicSystem::GetInstance()->PostMsgToQue(
				std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));

			AsyncReadHead();
		}
		catch (std::exception& e) {
			std::cerr << "Exception in AsyncReadBody: " << e.what() << std::endl;
		}
	});
}

void CSession::asyncReadFull(std::size_t max_length,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	memset(_data, 0, max_length);
	asyncReadLen(0, max_length, std::move(handler));
}

void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	auto self = shared_from_this();
	_socket.async_read_some(boost::asio::buffer(_data + read_len, total_len - read_len),
		[read_len, total_len, handler = std::move(handler), self](
			const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (ec) {
				handler(ec, read_len + bytes_transferred);
				return;
			}

			if (read_len + bytes_transferred >= total_len) {
				handler(ec, read_len + bytes_transferred);
				return;
			}

			self->asyncReadLen(read_len + bytes_transferred, total_len, handler);
		});
}

void CSession::DealExceptionSession()
{
	if (_cleanup_started.exchange(true))
		return;
	_server->ClearSession(_session_id);
}

LogicNode::LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode)
	: _session(std::move(session)), _recvnode(std::move(recvnode)) {}
