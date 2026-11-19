#pragma once
// 客户端连接的传输层：明文 TCP 或 TLS
//
// 上层协议（聊天 2+2 帧、资源 2+4 帧）完全不感知是否加密：
//   · 绑定时把 LowestSocket() 交给 acceptor
//   · 开始读之前调 AsyncHandshake()，明文连接会异步回调成功，调用方只写一条路径
//   · 之后只用 AsyncReadSome / AsyncWrite / Close
// 构造时 tls 为空就是明文，行为与改造前完全一致。

#include <boost/asio.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/ssl.hpp>
#include <chrono>
#include <functional>
#include <memory>
#include <utility>

#include "TlsContext.h"

using tcp = boost::asio::ip::tcp;

class TlsSocket {
public:
	//tls 为空 = 明文；非空时构造期取一次证书快照，热加载不影响这条连接
	TlsSocket(net::io_context& ioc, std::shared_ptr<TlsContext> tls);

	TlsSocket(const TlsSocket&) = delete;
	TlsSocket& operator=(const TlsSocket&) = delete;

	bool IsTls() const { return _context != nullptr; }
	//acceptor 绑这个；TLS 握手后返回 ssl_stream 内部同一个 socket，远端地址仍然可查
	tcp::socket& LowestSocket() { return _stream ? _stream->next_layer() : _socket; }
	//会话跨线程投递用它：socket 被移进 ssl_stream 后，原对象的执行器不能再用
	net::any_io_executor GetExecutor() const { return _executor; }

	//只在 socket 所属 IO 线程调用。超过 timeout_seconds 未完成握手就关掉底层 socket
	void AsyncHandshake(std::function<void(const boost::system::error_code&)> on_done,
		int timeout_seconds = 10);

	//可重入：读失败、写失败、心跳超时都会来关，重复调用无副作用
	void Close();

	template <typename MutableBufferSequence, typename Handler>
	void AsyncReadSome(const MutableBufferSequence& buffers, Handler&& handler) {
		//读「有多少给多少」是成员函数，asio 没有同名的自由函数
		if (_stream)
			_stream->async_read_some(buffers, std::forward<Handler>(handler));
		else
			_socket.async_read_some(buffers, std::forward<Handler>(handler));
	}

	template <typename ConstBufferSequence, typename Handler>
	void AsyncWrite(const ConstBufferSequence& buffers, Handler&& handler) {
		if (_stream)
			net::async_write(*_stream, buffers, std::forward<Handler>(handler));
		else
			net::async_write(_socket, buffers, std::forward<Handler>(handler));
	}

private:
	void AdoptTls(); //把裸 socket 交给 ssl_stream，只执行一次

	tcp::socket _socket; //accept 用；TLS 模式下随后被移入 _stream
	std::unique_ptr<ssl::stream<tcp::socket>> _stream;
	std::shared_ptr<ssl::context> _context; //连接生命周期内固定
	net::any_io_executor _executor; //构造时留一份，socket 被移走后仍然可用
	net::steady_timer _handshake_deadline; //握手超时，防止半开连接长期占着会话
};
