#include "CServer.h"
#include "IOContextPool.h"
#include <iostream>

CServer::CServer(boost::asio::io_context& io_context, short port)
	: _port(port), _acceptor(io_context, tcp::endpoint(tcp::v4(), port)) //监听所有IPv4地址
{
	std::cout << "Resource server listen on port: " << _port << std::endl;
	StartAccept(); //先挂第一个accept
}

CServer::~CServer()
{
	Stop(); //析构时保证acceptor与会话都关掉
}

void CServer::HandleAccept(std::shared_ptr<CSession> session, const boost::system::error_code& error)
{
	if (!error) { //连接建立成功
		//停服时acceptor可能已经交出最后一个连接，这里直接关掉，不再登记
		if (_stopped.load()) {
			session->Close();
			return;
		}
		{
			std::lock_guard<std::mutex> lock(_mutex);
			_sessions.emplace(session->GetSessionId(), session); //先登记
		}
		session->Start(); //再开始读，避免首帧先于登记到达
	}
	else { //accept失败通常是acceptor已关闭
		std::cout << "session accept failed, error is " << error.message() << std::endl;
	}

	//一次只挂一个accept，处理完继续挂下一个，形成接受循环
	if (!_stopped.load())
		StartAccept();
}

void CServer::StartAccept()
{
	if (_stopped.load()) //停服后不再挂新连接
		return;

	auto& io_context = IOContextPool::GetInstance()->GetIOContext(); //轮询一个IO线程
	auto session = std::make_shared<CSession>(io_context, this); //新会话属于该IO线程
	_acceptor.async_accept(session->GetSocket(),
		std::bind(&CServer::HandleAccept, this, session, std::placeholders::_1)); //异步等待下一个连接
}

void CServer::ClearSession(std::string session_id)
{
	std::lock_guard<std::mutex> lock(_mutex); //会话表可能被多个IO线程访问
	_sessions.erase(session_id); //释放shared_ptr，连接对象随之析构
}

bool CServer::CheckValid(std::string session_id)
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _sessions.find(session_id) != _sessions.end(); //读回调据此判断会话是否还活着
}

void CServer::Stop()
{
	if (_stopped.exchange(true)) //只停一次
		return;

	boost::system::error_code ec;
	_acceptor.cancel(ec); //取消未完成的accept
	_acceptor.close(ec); //释放监听端口

	std::vector<std::shared_ptr<CSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (auto& entry : _sessions) //先复制一份，避免持锁回调
			sessions.push_back(entry.second);
	}

	for (auto& session : sessions) {
		session->Close(); //通知socket关闭
		session->DealExceptionSession(); //从会话表移除
	}
}
