#include "CServer.h"
#include "ConfigMgr.h"
#include "PasswordPool.h"
#include "RedisMgr.h"
#include "TlsContext.h"
#include "Log.h"

int main()
{
	Log::Init("GateServer"); //日志最先初始化，后面所有启动与业务日志都走这里
	Log::Guard log_guard; //任何 return 路径都会刷盘
	auto& gCfgMgr = ConfigMgr::Inst(); //获取配置管理器实例, 用于读取配置文件中的数据
	//从配置管理器中获取GateServer的Port配置项, 并将其转换为unsigned short类型
    std::string gate_port_str = gCfgMgr["GateServer"]["Port"];
    unsigned short gate_port = atoi(gate_port_str.c_str());

	//证书两项都填才启用 HTTPS：只填一项属于配置错误，按启动失败处理
	std::string cert_file = gCfgMgr["GateServer"]["SslCert"];
	std::string key_file = gCfgMgr["GateServer"]["SslKey"];
	std::shared_ptr<TlsContext> tls;
	if (!cert_file.empty() || !key_file.empty()) {
		if (cert_file.empty() || key_file.empty()) {
			LOG_ERROR << "SslCert and SslKey must be set together, refused to start";
			return EXIT_FAILURE;
		}
		//证书读不出来不能悄悄退回明文：密码与验证码会以明文过公网
		tls = TlsContext::Load(cert_file, key_file);
		if (!tls) {
			LOG_ERROR << "tls init failed, server not started";
			return EXIT_FAILURE;
		}
	}

    try
    {
        //服务器监听的端口号, 取自 config.ini 的 [GateServer].Port
        unsigned short port = gate_port;
		net::io_context ioc{ 1 }; //使用一个线程来运行io_context
        //创建一个信号集, 用于捕捉SIGINT和SIGTERM信号, 以便优雅地关闭服务器
		boost::asio::signal_set signals(ioc, SIGINT, SIGTERM); 
		//异步等待信号, 当捕捉到信号时, 停止io_context
        signals.async_wait([&ioc](const boost::system::error_code& error, int signal_number) {
            if (error) {
                return;
            }
            ioc.stop();
        });

		if (tls) {
			//续期后覆盖证书文件即可，进程按配置的间隔自己换新；没写这一项时用 60 秒
			std::string reload_str = gCfgMgr["GateServer"]["SslReloadSeconds"];
			const unsigned reload_seconds = reload_str.empty()
				? 60u
				: static_cast<unsigned>(atoi(reload_str.c_str()));
			tls->StartWatch(ioc, reload_seconds);
		}

		//创建服务器实例, 开始监听端口, 接受连接
        std::make_shared<CServer>(ioc, port, tls)->Start();
		if (tls)
			spdlog::info("https listening on :{} cert={} expires_in_days={}",
				port, tls->CertFile(), tls->ExpiresInDays());
		else
			spdlog::info("http listening on :{}", port);
		ioc.run(); //运行io_context, 处理异步事件
		spdlog::info("gate stopped, shutting down");

		//ioc 已停止，未回投的响应不会再发；这里只负责 join 工作线程，
		//避免进程退出时还有哈希在跑（每个占用 64 MiB）
		PasswordPool::Instance().Stop();
    }
    catch (std::exception const& e)
    {
        LOG_ERROR << "Error: " << e.what();
        return EXIT_FAILURE;
    }
}
