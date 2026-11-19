#include "LogicSystem.h"
#include "ArchiveJanitor.h"
#include <thread>
#include <mutex>
#include <csignal>
#include "IOContextPool.h"
#include "CServer.h"
#include "ConfigMgr.h"
#include "RedisMgr.h"
#include "MysqlDao.h"
#include "ChatServiceImpl.h"
#include "StatusGrpcClient.h"
#include "TlsContext.h"
#include "Log.h"

int main()
{
	Log::Init("ChatServer"); //日志最先初始化，后面所有启动与业务日志都走这里
	Log::Guard log_guard; //任何 return 路径都会刷盘
	auto& cfg = ConfigMgr::Inst(); //获取配置管理器实例
	try {
		auto pool = IOContextPool::GetInstance(); //获取IOContextPool单例实例

		//启动时把数据库连接池建好: tags 补列这类一次性 DDL 在启动阶段完成,
		//不落到第一个登录请求上, 失败也能在启动日志里看到
		MysqlDao::GetInstance();
	    Defer derfer ([]() {
                RedisMgr::GetInstance()->Close();
            });

	    boost::asio::io_context io_context;
	    auto port_str = cfg["SelfServer"]["Port"];

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
	            LOG_ERROR << "tls init failed, chat server not started";
	            return EXIT_FAILURE;
	        }
	        const std::string reload_str = cfg["SelfServer"]["SslReloadSeconds"];
	        tls->StartWatch(io_context, reload_str.empty() ? 60u
	            : static_cast<unsigned>(atoi(reload_str.c_str())));
	    }

	    //创建Cserver智能指针
	    auto pointer_server = std::make_shared<CServer>(io_context, atoi(port_str.c_str()), tls);

		//从配置文件中获取服务器的主机地址和RPC端口号，构成服务器的监听地址
		std::string server_address(cfg["SelfServer"]["Host"] + ":" + cfg["SelfServer"]["RPCPort"]);
		ChatServiceImpl service; //创建ChatServiceImpl服务对象，负责处理RPC请求
		grpc::ServerBuilder builder; //创建gRPC服务器构建器
		//监听端口和添加服务
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);
	    service.RegisterServer(pointer_server); //将Cserver注册给ChatServiceImpl服务对象，方便处理踢人请求

		//构建并启动gRPC服务器
		std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        //端口占用等原因可能使BuildAndStart返回空指针，启动失败时直接退出，不能继续调用Wait或Shutdown
        if (!server) {
            LOG_ERROR << "Cannot start gRPC server on " << server_address;
            return EXIT_FAILURE;
        }
		spdlog::info("grpc listening on {}", server_address);

		//单独启动一个线程处理grpc服务
		std::thread  grpc_server_thread([&server]() {
			server->Wait();
			});

		//注册要放在 gRPC 服务开始监听之后：否则状态服务可能先分配登录，
		//而踢人或消息转发会连到一个还没 listening 的端口
		pointer_server->StartTimer();

		//创建一个信号集，监听SIGINT和SIGTERM信号
		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM);
		//当接收到SIGINT或SIGTERM信号时
		signals.async_wait([&io_context, &server](auto, auto) {
			server->Shutdown(); //优雅关闭gRPC服务器，停止接收新请求并处理完现有请求
			io_context.stop(); //停止io_context的事件循环，结束主线程
			});

	    //将Cserver注册给逻辑类方便以后清除连接
	    LogicSystem::GetInstance()->SetServer(pointer_server);

		//归档线程独立于事件循环：每天搬一批超过保留期的消息
		ArchiveJanitor archive_janitor;
		archive_janitor.Start();

		io_context.run(); //运行io_context，开始处理事件循环，直到接收到停止信号
		spdlog::info("tcp stopped, shutting down");
		archive_janitor.Stop(); //先把搬运停下，再让连接池和数据库收尾

		grpc_server_thread.join(); //等待grpc服务线程结束
	    LogicSystem::GetInstance()->Stop(); //先排空业务线程，之后再清理会话和共享资源
	    pointer_server->Stop(); //停止 accept/timer 并清理会话；析构时重复调用是安全的
	    //停服会把每条连接的释放任务排进队列，这里等它们真正发给状态服务再退出
	    StatusGrpcClient::GetInstance()->Shutdown();
	    pool->StopAfterDrain(); //让已投递的socket关闭和取消回调先完成，再回收会话IO线程
	    return 0;
	}
	catch (std::exception& e) {
		LOG_ERROR << "Exception: " << e.what();
	}
}

