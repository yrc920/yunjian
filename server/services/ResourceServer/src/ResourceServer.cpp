#include "CServer.h"
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "IOContextPool.h"
#include "LogicSystem.h"
#include "RedisMgr.h"
#include "StorageJanitor.h"
#include "TlsContext.h"
#include <boost/asio.hpp>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include "Log.h"

int main()
{
	try {
		//配置缺失会直接抛异常，宁可启动失败也不要用默认端口和空存储目录
	Log::Init("ResourceServer"); //日志最先初始化，后面所有启动信息都走这里
	Log::Guard log_guard; //任何 return 路径都会刷盘
		auto& cfg = ConfigMgr::Inst();
		const int port = std::atoi(cfg["SelfServer"]["Port"].c_str()); //监听端口
		if (port <= 0) {
			LOG_ERROR << "invalid ResourceServer port";
			return EXIT_FAILURE;
		}

		//主线程只跑acceptor与信号；连接读写分散到IOContextPool
		auto pool = IOContextPool::GetInstance();
		boost::asio::io_context io_context; //acceptor所在事件循环

		//证书两项都填才启用 TLS：只填一项按配置错误处理，不静默退回明文
		std::shared_ptr<TlsContext> tls;
		const std::string cert_file = cfg["SelfServer"]["SslCert"];
		const std::string key_file = cfg["SelfServer"]["SslKey"];
		if (!cert_file.empty() || !key_file.empty()) {
			if (cert_file.empty() || key_file.empty()) {
				LOG_ERROR << "SslCert and SslKey must be set together, refused to start";
				return EXIT_FAILURE;
			}
			tls = TlsContext::Load(cert_file, key_file);
			if (!tls) {
				LOG_ERROR << "tls init failed, resource server not started";
				return EXIT_FAILURE;
			}
			const std::string reload_str = cfg["SelfServer"]["SslReloadSeconds"];
			tls->StartWatch(io_context, reload_str.empty() ? 60u
				: static_cast<unsigned>(std::atoi(reload_str.c_str())));
		}

		auto server = std::make_shared<CServer>(io_context, static_cast<short>(port), tls); //开始监听
		StorageJanitor janitor; //清理上传中断留下的 .part，独立线程不影响接入
		janitor.Start();

		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM); //Ctrl-C/系统终止
		signals.async_wait([&io_context](auto, auto) {
			io_context.stop(); //收到信号后退出run
		});

		io_context.run(); //阻塞到这里，直到停服
		spdlog::info("tcp stopped, shutting down");

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
		LOG_ERROR << "ResourceServer exception: " << e.what();
	}
	return EXIT_FAILURE;
}
