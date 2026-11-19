#include "TlsSocket.h"

TlsSocket::TlsSocket(net::io_context& ioc, std::shared_ptr<TlsContext> tls)
	: _socket(ioc)
	, _context(tls ? tls->Get() : nullptr)
	, _executor(_socket.get_executor())
	, _handshake_deadline(_socket.get_executor()) {}

void TlsSocket::AdoptTls() {
	if (_stream || !_context)
		return;
	_stream = std::make_unique<ssl::stream<tcp::socket>>(std::move(_socket), *_context);
}

void TlsSocket::AsyncHandshake(std::function<void(const boost::system::error_code&)> on_done,
	int timeout_seconds) {
	if (!_context) {
		//明文连接没有握手：仍然异步回调，调用方只写一条路径
		net::post(_executor, [done = std::move(on_done)]() mutable {
			done(boost::system::error_code{});
			});
		return;
	}

	AdoptTls();
	TlsSocket* self = this; //由调用方持有的会话保证生命周期，回调不会晚于会话析构
	_handshake_deadline.expires_after(std::chrono::seconds(timeout_seconds));
	_handshake_deadline.async_wait([self](const boost::system::error_code& ec) {
		if (ec)
			return; //握手正常完成时定时器已被取消
		//对端连上却一直不发 ClientHello：关掉底层 socket，挂起的握手会以 operation_aborted 返回
		boost::system::error_code ignored;
		self->LowestSocket().close(ignored);
		});

	_stream->async_handshake(ssl::stream_base::server,
		[self, done = std::move(on_done)](const boost::system::error_code& ec) mutable {
			self->_handshake_deadline.cancel();
			done(ec);
		});
}

void TlsSocket::Close() {
	_handshake_deadline.cancel();
	boost::system::error_code ec;
	LowestSocket().close(ec); //TLS 下关的是内层 socket，挂起的读写会以 operation_aborted 返回
}
