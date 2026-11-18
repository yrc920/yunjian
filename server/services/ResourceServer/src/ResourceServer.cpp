#include "CServer.h"
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "IOContextPool.h"
#include "LogicSystem.h"
#include "RedisMgr.h"
#include "StorageJanitor.h"
#include <boost/asio.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>

int main()
{
	try {
		//配置缺失会直接抛异常，宁可启动失败也不要用默认端口和空存储目录
		auto& cfg = ConfigMgr::Inst();
		const int port = std::atoi(cfg["SelfServer"]["Port"].c_str()); //监听端口
		if (port <= 0) {
			std::cerr << "invalid ResourceServer port" << std::endl;
			return EXIT_FAILURE;
		}

		//主线程只跑acceptor与信号；连接读写分散到IOContextPool
		auto pool = IOContextPool::GetInstance();
		boost::asio::io_context io_context; //acceptor所在事件循环
		auto server = std::make_shared<CServer>(io_context, static_cast<short>(port)); //开始监听
		StorageJanitor janitor; //清理上传中断留下的 .part，独立线程不影响接入
		janitor.Start();

		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM); //Ctrl-C/系统终止
		signals.async_wait([&io_context](auto, auto) {
			io_context.stop(); //收到信号后退出run
		});

		io_context.run(); //阻塞到这里，直到停服

		//先停止接入与会话，再结束业务线程，最后回收网络线程和Redis连接
		server->Stop();
		janitor.Stop();
		LogicSystem::GetInstance()->Stop();
		FileSystem::GetInstance()->Stop();
		pool->StopAfterDrain();
		RedisMgr::GetInstance()->Close();
		return 0;
	}
	catch (std::exception& e) { //配置/端口/依赖异常
		std::cerr << "ResourceServer exception: " << e.what() << std::endl;
	}
	return EXIT_FAILURE;
}
