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
	CServer(boost::asio::io_context& io_context, short port);
	~CServer();

	void ClearSession(std::string session_id);
	bool CheckValid(std::string session_id);
	void Stop();

private:
	void HandleAccept(std::shared_ptr<CSession> session, const boost::system::error_code& error);
	void StartAccept();

	short _port;
	tcp::acceptor _acceptor;
	std::map<std::string, std::shared_ptr<CSession>> _sessions;
	std::mutex _mutex;
	std::atomic<bool> _stopped{false};
};
