#include "CServer.h"
#include "IOContextPool.h"
#include <iostream>

CServer::CServer(boost::asio::io_context& io_context, short port)
	: _port(port), _acceptor(io_context, tcp::endpoint(tcp::v4(), port))
{
	std::cout << "Resource server listen on port: " << _port << std::endl;
	StartAccept();
}

CServer::~CServer()
{
	Stop();
}

void CServer::HandleAccept(std::shared_ptr<CSession> session, const boost::system::error_code& error)
{
	if (!error) {
		if (_stopped.load()) {
			session->Close();
			return;
		}
		//先登记再启动读取，避免读回调先于登记完成被CheckValid判为失效
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_sessions.emplace(session->GetSessionId(), session);
		}
		session->Start();
	}
	else {
		std::cout << "session accept failed, error is " << error.message() << std::endl;
	}

	if (!_stopped.load())
		StartAccept();
}

void CServer::StartAccept()
{
	if (_stopped.load())
		return;

	auto& io_context = IOContextPool::GetInstance()->GetIOContext();
	auto session = std::make_shared<CSession>(io_context, this);
	_acceptor.async_accept(session->GetSocket(),
		std::bind(&CServer::HandleAccept, this, session, std::placeholders::_1));
}

void CServer::ClearSession(std::string session_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	_sessions.erase(session_id);
}

bool CServer::CheckValid(std::string session_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _sessions.find(session_id) != _sessions.end();
}

void CServer::Stop()
{
	if (_stopped.exchange(true))
		return;

	boost::system::error_code ec;
	_acceptor.cancel(ec);
	_acceptor.close(ec);

	std::vector<std::shared_ptr<CSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (auto& entry : _sessions)
			sessions.push_back(entry.second);
	}

	for (auto& session : sessions) {
		session->Close();
		session->DealExceptionSession();
	}
}
