#pragma once
// 监听一个端口, 创建acceptor, 接受所有过来连接的socket
//
// 是否加密由构造时传入的 TlsContext 决定：为空按明文 HTTP 服务，非空则每条连接先握手。
// 一个监听端口只有一种协议，因此明文与 HTTPS 不能共用同一个端口。

#include "const.h"
#include "TlsContext.h"

class CServer : public std::enable_shared_from_this<CServer>
{
public:
	CServer(net::io_context& ioc, unsigned short& port, std::shared_ptr<TlsContext> tls);
	void Start(); //开始监听连接

private:
	tcp::acceptor _acceptor; //监听器, 用于监听端口, 接受连接
	std::shared_ptr<TlsContext> _tls; //空 = 明文
};
