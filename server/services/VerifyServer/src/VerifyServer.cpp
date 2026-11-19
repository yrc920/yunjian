#include "const.h"
#include "ConfigMgr.h"
#include "EmailSender.h"
#include "VerifyServiceImpl.h"

#include <boost/asio.hpp>
#include "Log.h"

int main()
{
	Log::Init("VerifyServer"); //日志最先初始化，后面所有启动信息都走这里
	Log::Guard log_guard; //任何 return 路径都会刷盘
	auto& cfg = ConfigMgr::Inst(); //获取配置管理器实例（读取当前目录下的 config.ini）
	std::string server_address(cfg["VerifyServer"]["Host"] + ":" + cfg["VerifyServer"]["Port"]);

	try {
		EmailSender::InitSSL(); //SMTP 走 TLS，先初始化 OpenSSL 环境

		VerifyServiceImpl service; //gRPC 服务实现
		grpc::ServerBuilder builder;

		//监听端口并注册服务
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);

		std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        //端口占用等原因可能使BuildAndStart返回空指针，启动失败时直接退出，不能继续调用Wait或Shutdown
        if (!server) {
            LOG_ERROR << "Cannot start gRPC server on " << server_address;
            return EXIT_FAILURE;
        }
		spdlog::info("grpc listening on {}", server_address);

		boost::asio::io_context io_context;
		boost::asio::signal_set signals(io_context, SIGINT, SIGTERM); //捕获 Ctrl+C 与终止信号

		signals.async_wait([&server](const boost::system::error_code& error, int /*signal_number*/) {
			if (!error) {
				LOG_INFO << "Shutting down server...";
				server->Shutdown(); //优雅关闭，等待在途 RPC 结束
			}
			});

		std::thread signal_thread([&io_context]() { io_context.run(); }); //单独运行信号处理，主线程在Wait中等待服务结束

		server->Wait(); //阻塞直到服务器关闭
		io_context.stop();
        //信号线程引用了局部io_context，必须等线程退出后才能离开作用域，不能让线程访问已析构的对象
        signal_thread.join();
		EmailSender::UninitSSL();
	}
	catch (std::exception const& e) {
		LOG_ERROR << "Error: " << e.what();
		return EXIT_FAILURE;
	}
	return 0;
}
