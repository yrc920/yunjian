#pragma once
// 资源服务器监听器：接受连接并管理会话表
//

#include <boost/asio.hpp>
#include "CSession.h"
#include <memory>
#include <map>
#include <mutex>
#include <atomic>

class CServer : public std::enable_shared_from_this<CServer>
{
public:
	//tls 为空时客户端连接为明文 TCP；非空则每条连接先握手
	CServer(boost::asio::io_context& io_context, short port, std::shared_ptr<TlsContext> tls);
	~CServer();

	void ClearSession(std::string session_id); //断连时从会话表移除
	bool CheckValid(std::string session_id); //读回调确认会话仍有效
	void Stop(); //停accept并关闭所有会话

private:
	void HandleAccept(std::shared_ptr<CSession> session, const boost::system::error_code& error); //accept完成
	void StartAccept(); //从IOContextPool取执行器挂下一个accept

	short _port;
	tcp::acceptor _acceptor;
	std::map<std::string, std::shared_ptr<CSession>> _sessions; //会话ID -> 会话
	std::mutex _mutex; //读写会话表都从这里过
	std::atomic<bool> _stopped{false}; //停服后不再挂新的accept
	std::shared_ptr<TlsContext> _tls; //空 = 明文接入
};
