#pragma once
// TLS 证书与私钥：加载、到期提示、按文件变更热加载
//
// 证书由外部签发（腾讯云免费 DV 或 Let's Encrypt）。续期时覆盖两个文件即可，
// 进程按 ReloadSeconds 比对 mtime 重建 ssl::context；已建立的连接继续用旧上下文，
// 所以热加载不会打断正在传输的数据。
//
// 三个入口共用：GateServer 的 HTTPS、ChatServer 的 TCP、ResourceServer 的文件通道。

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <filesystem>
#include <memory>
#include <shared_mutex>
#include <string>

namespace net = boost::asio;
namespace ssl = boost::asio::ssl;

class TlsContext : public std::enable_shared_from_this<TlsContext> {
public:
	//配置了证书却读不出来属于致命错误：返回空指针，由调用方终止启动，不静默退回明文
	static std::shared_ptr<TlsContext> Load(const std::string& cert_file, const std::string& key_file);

	//每条连接取一次快照：重载后新连接用新证书，老连接不受影响
	std::shared_ptr<ssl::context> Get() const;

	//定时比对证书文件，被替换后自动重载；interval_seconds 为 0 表示不检查
	void StartWatch(net::io_context& ioc, unsigned interval_seconds);
	void ReloadIfChanged();
	//剩余不足 15 天时每天提醒一次：免费 DV 证书只有 90 天，过期后客户端会直接连不上
	void WarnIfExpiring();

	std::string CertFile() const;
	int ExpiresInDays() const;

private:
	TlsContext(std::string cert_file, std::string key_file);

	bool Build();                  //读文件重建 ssl::context，成功后原子替换
	std::string Signature() const; //两个文件的 mtime 与大小，用来判断是否被替换

	mutable std::shared_mutex _mutex;
	std::shared_ptr<ssl::context> _context;
	std::string _cert_file;
	std::string _key_file;
	std::string _signature;        //当前生效文件的签名
	std::string _failed_signature; //加载失败过的签名，避免每分钟重复刷同一条错误
	int _expires_in_days = 0;
	int _warned_day = -1;          //上次提示到期时算出的剩余天数
};
