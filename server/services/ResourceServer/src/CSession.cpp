#include "CSession.h"
#include "CServer.h"
#include "LogicSystem.h"
#include <iostream>
#include <cstring>
#include "Log.h"

CSession::CSession(boost::asio::io_context& io_context, CServer* server, std::shared_ptr<TlsContext> tls)
	: _socket(io_context, std::move(tls)), _server(server)
{
	boost::uuids::uuid uuid = boost::uuids::random_generator()(); //每个连接一个独立UUID
	_session_id = boost::uuids::to_string(uuid); //逻辑线程按它做哈希分派
	_recv_head_node = std::make_shared<MsgNode>(HEAD_TOTAL_LEN); //6字节头部缓冲，循环复用
}

CSession::~CSession() = default;

tcp::socket& CSession::GetSocket()
{
	return _socket.LowestSocket();
}

std::string& CSession::GetSessionId()
{
	return _session_id;
}

bool CSession::BindUser(int uid)
{
	if (uid <= 0)
		return false; //非法uid不绑定

	int expected = 0;
	if (_user_uid.compare_exchange_strong(expected, uid))
		return true; //第一个请求，绑定成功
	return expected == uid; //已经绑过：只有同一个uid才放行
}

void CSession::Start()
{
	auto self = shared_from_this();
	boost::asio::post(_socket.GetExecutor(), [self]() {
		if (self->IsClosed())
			return;
		//TLS 连接先握手；明文连接这里会立即回调成功，后面只有一条路径
		self->_socket.AsyncHandshake([self](const boost::system::error_code& ec) {
			if (ec) {
				if (ec != boost::asio::error::operation_aborted)
					LOG_DEBUG << "tls handshake failed: " << ec.message();
				//握手失败还没开始读，读回调不会再来，这里直接把会话摘掉，别等心跳超时
				self->Close();
				self->DealExceptionSession();
				return;
			}
			if (!self->IsClosed())
				self->AsyncReadHead();
			});
	});
}

void CSession::Close()
{
	//读失败、写失败和停服都可能关闭同一连接，exchange保证只投递一次
	if (_b_close.exchange(true))
		return;

	auto self = shared_from_this();
	boost::asio::post(_socket.GetExecutor(), [self]() {
		self->_socket.Close();
	});
}

void CSession::Send(std::string msg, std::uint16_t msgid)
{
	if (IsClosed()) //已关闭的连接不再入队
		return;

	//长度异常说明序列化出错，断开并清理会话，避免把坏帧发给对端
	if (msg.empty() || msg.size() > MAX_MESSAGE_LEN) {
		Close(); //先关socket，后续回调不会再进入
		DealExceptionSession(); //解绑Redis里的在线状态
		return;
	}

	//同时统计已入队和等待进入IO线程的发送任务，避免高并发下载时在io_context中积压
	if (_pending_sends.fetch_add(1) >= MAX_SENDQUE) {
		_pending_sends.fetch_sub(1); //超限要归还刚加的计数
		Close(); //直接断开，防止发送任务撑爆内存
		DealExceptionSession(); //按uid解绑，不影响该用户的新会话
		return;
	}

	auto self = shared_from_this(); //lambda持有会话，异步写期间对象不会被释放
	//帧的组装与socket写都在IO线程进行，逻辑线程不能直接碰socket
	boost::asio::post(_socket.GetExecutor(), [self, msg = std::move(msg), msgid]() mutable {
		self->SendOnIO(std::move(msg), msgid); //进入发送队列或立即发起异步写
	});
}

void CSession::SendOnIO(std::string msg, std::uint16_t msgid)
{
	if (IsClosed()) { //排队期间可能已被关闭
		_pending_sends.fetch_sub(1); //本任务还没入队，只归还自身计数
		return;
	}

	const bool writing = !_send_que.empty(); //队列非空说明已有异步写
	_send_que.push(std::make_shared<SendNode>(msg.data(), msg.size(), msgid)); //2+4头 + JSON
	if (writing)
		return; //已有写操作在推进，等HandleWrite续写

	auto& node = _send_que.front(); //队列空，当前就是队首
	_socket.AsyncWrite(boost::asio::buffer(node->_data, node->_total_len),
		std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_from_this())); //写完整帧
}

void CSession::HandleWrite(const boost::system::error_code& error,
	std::shared_ptr<CSession> shared_self)
{
	if (error) { //写失败，连接不可继续
		LOG_DEBUG << "handle write failed, error is " << error.message();
		Close(); //标记关闭并投递socket.close
		DealExceptionSession(); //从会话表删除
		//异步写完成前不能清空队首缓冲区，此时回调已经返回，可以释放队列
		std::queue<std::shared_ptr<SendNode>> empty;
		_pending_sends.fetch_sub(_send_que.size()); //扣掉仍在队列里的任务
		_send_que.swap(empty);
		return;
	}

	_send_que.pop(); //队首已经写完
	_pending_sends.fetch_sub(1); //归还这一条的计数

	if (!_send_que.empty() && !IsClosed()) { //还有排队帧且连接仍开着
		auto& node = _send_que.front();
		_socket.AsyncWrite(boost::asio::buffer(node->_data, node->_total_len),
			std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_self)); //继续下一帧
	}
}

void CSession::AsyncReadHead()
{
	auto self = shared_from_this(); //回调期间保持会话存活
	asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code& ec,
		std::size_t bytes_transferred) {
		try {
			if (ec) { //连接已断
				Close();
				DealExceptionSession();
				return;
			}

			if (bytes_transferred < HEAD_TOTAL_LEN) { //没读满6字节，按异常连接处理
				Close();
				DealExceptionSession();
				return;
			}

			//关闭后可能还有已投递的回调，先确认会话仍在表中
			if (!_server->CheckValid(_session_id)) {
				Close();
				return;
			}

			_recv_head_node->Clear(); //清掉上一帧残留
			memcpy(_recv_head_node->_data, _data, bytes_transferred); //把头部拷到独立缓冲

			std::uint16_t msg_id = 0;
			memcpy(&msg_id, _recv_head_node->_data, HEAD_ID_LEN); //前2字节是ID
			msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id); //转主机序

			std::uint32_t msg_len = 0;
			memcpy(&msg_len, _recv_head_node->_data + HEAD_ID_LEN, HEAD_DATA_LEN); //后4字节是长度
			msg_len = boost::asio::detail::socket_ops::network_to_host_long(msg_len); //转主机序

			//先拒绝空包和超过缓冲区的长度，再分配RecvNode，避免异常长度撑爆内存
			if (msg_id == 0 || msg_len == 0 || msg_len > MAX_MESSAGE_LEN) {
				LOG_WARN << "invalid header, msg_id is " << msg_id
					<< ", msg_len is " << msg_len;
				Close();
				DealExceptionSession();
				return;
			}

			_recv_msg_node = std::make_shared<RecvNode>(msg_len, msg_id); //按实际长度分配包体
			AsyncReadBody(msg_len); //继续读包体
		}
		catch (std::exception& e) {
			LOG_ERROR << "Exception in AsyncReadHead: " << e.what();
		}
	});
}

void CSession::AsyncReadBody(std::size_t total_len)
{
	auto self = shared_from_this(); //回调持有会话
	asyncReadFull(total_len, [self, this, total_len](
		const boost::system::error_code& ec, std::size_t bytes_transferred) {
		try {
			if (ec) { //连接断开
				Close();
				DealExceptionSession();
				return;
			}

			if (bytes_transferred < total_len) { //包体不完整
				Close();
				DealExceptionSession();
				return;
			}

			if (!_server->CheckValid(_session_id)) { //会话已被清理
				Close();
				return;
			}

			memcpy(_recv_msg_node->_data, _data, bytes_transferred); //把包体拷进消息节点
			_recv_msg_node->_cur_len = bytes_transferred; //记录实际长度
			_recv_msg_node->_data[_recv_msg_node->_total_len] = '\0'; //补字符串结尾

			//按会话分派逻辑线程，分片顺序由客户端等待上一片回包保证
			LogicSystem::GetInstance()->PostMsgToQue(
				std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));

			AsyncReadHead(); //继续读下一帧头
		}
		catch (std::exception& e) {
			LOG_ERROR << "Exception in AsyncReadBody: " << e.what();
		}
	});
}

void CSession::asyncReadFull(std::size_t max_length,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	memset(_data, 0, max_length); //清掉上一帧残留
	asyncReadLen(0, max_length, std::move(handler)); //从0开始补齐
}

void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	auto self = shared_from_this(); //递归回调期间保持存活
	_socket.AsyncReadSome(boost::asio::buffer(_data + read_len, total_len - read_len),
		[read_len, total_len, handler = std::move(handler), self](
			const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (ec) { //读失败，把已读长度一起带回
				handler(ec, read_len + bytes_transferred);
				return;
			}

			if (read_len + bytes_transferred >= total_len) { //已经读满
				handler(ec, read_len + bytes_transferred);
				return;
			}

			self->asyncReadLen(read_len + bytes_transferred, total_len, handler); //继续读剩余部分
		});
}

void CSession::DealExceptionSession()
{
	if (_cleanup_started.exchange(true)) //已经清理过
		return;
	_server->ClearSession(_session_id); //只从表里删一次
}

LogicNode::LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode)
	: _session(std::move(session)), _recvnode(std::move(recvnode)) {}
