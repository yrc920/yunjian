#include "CServer.h"
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "IOContextPool.h"
#include "LogicSystem.h"
#include "RedisMgr.h"
#include <boost/asio.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>

int main()
{
	try {
		auto& cfg = ConfigMgr::Inst();
		const int port = std::atoi(cfg["SelfServer"]["Port"].c_str());
		if (port <= 0) {
			std::cerr << "invalid ResourceServer port" << std::endl;
			return EXIT_FAILURE;
		}

		auto pool = IOContextPool::GetInstance();
		boost::asio::io_context io_context;
		auto server = std::make_shared<CServer>(io_context, static_cast<short>(port));

		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
		signals.async_wait([&io_context](auto, auto) {
			io_context.stop();
		});

		io_context.run();

		//先停止接入与会话，再结束业务线程，最后回收网络线程和Redis连接
		server->Stop();
		LogicSystem::GetInstance()->Stop();
		FileSystem::GetInstance()->Stop();
		pool->StopAfterDrain();
		RedisMgr::GetInstance()->Close();
		return 0;
	}
	catch (std::exception& e) {
		std::cerr << "ResourceServer exception: " << e.what() << std::endl;
	}
	return EXIT_FAILURE;
}
