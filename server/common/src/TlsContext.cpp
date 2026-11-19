#include "TlsContext.h"
#include "Log.h"

#include <openssl/asn1.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace fs = std::filesystem;

namespace {

//证书主体（CN）与剩余天数：免费 DV 证书只有 90 天，日志里要能提前看到到期
void ReadCertInfo(SSL_CTX* native, std::string& subject, int& days) {
	X509* cert = SSL_CTX_get0_certificate(native);
	if (!cert)
		return;
	char cn[256] = { 0 };
	X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName, cn, sizeof(cn) - 1);
	subject = cn;
	const ASN1_TIME* not_after = X509_get0_notAfter(cert);
	int seconds = 0;
	if (not_after && ASN1_TIME_diff(&days, &seconds, nullptr, not_after) != 1)
		days = 0;
}

void WatchTick(std::shared_ptr<TlsContext> ctx, std::shared_ptr<net::steady_timer> timer, unsigned seconds) {
	timer->expires_after(std::chrono::seconds(seconds));
	timer->async_wait([ctx, timer, seconds](const boost::system::error_code& ec) {
		if (ec)
			return; //定时器被取消，进程正在退出
		ctx->ReloadIfChanged();
		ctx->WarnIfExpiring();
		WatchTick(ctx, timer, seconds);
	});
}

} // namespace

TlsContext::TlsContext(std::string cert_file, std::string key_file)
	: _cert_file(std::move(cert_file)), _key_file(std::move(key_file)) {}

std::shared_ptr<TlsContext> TlsContext::Load(const std::string& cert_file, const std::string& key_file) {
	std::shared_ptr<TlsContext> ctx(new TlsContext(cert_file, key_file));
	if (!ctx->Build())
		return nullptr;
	return ctx;
}

std::shared_ptr<ssl::context> TlsContext::Get() const {
	std::shared_lock lock(_mutex);
	return _context;
}

std::string TlsContext::CertFile() const {
	std::shared_lock lock(_mutex);
	return _cert_file;
}

int TlsContext::ExpiresInDays() const {
	std::shared_lock lock(_mutex);
	return _expires_in_days;
}

std::string TlsContext::Signature() const {
	std::string sig;
	for (const std::string* path : { &_cert_file, &_key_file }) {
		std::error_code ec;
		//size 与 mtime 的原始类型是平台相关的宽整数，先收敛成 long long 再转字符串
		const auto size = static_cast<long long>(fs::file_size(*path, ec));
		const auto mtime = static_cast<long long>(fs::last_write_time(*path, ec).time_since_epoch().count());
		sig += std::to_string(size) + ":" + std::to_string(mtime) + ";";
	}
	return sig;
}

bool TlsContext::Build() {
	const std::string sig = Signature();
	auto ctx = std::make_shared<ssl::context>(ssl::context::tls_server);
	try {
		//只留 TLS 1.2/1.3：1.0/1.1 已被主流客户端放弃，Qt 6 默认也不会协商
		ctx->set_options(ssl::context::default_workarounds | ssl::context::no_sslv2
			| ssl::context::no_sslv3 | ssl::context::no_tlsv1 | ssl::context::no_tlsv1_1);
		//链文件要 fullchain（叶子 + 中间 CA）：只放叶子时部分客户端会报证书链不完整
		ctx->use_certificate_chain_file(_cert_file);
		ctx->use_private_key_file(_key_file, ssl::context::pem);
	}
	catch (const std::exception& e) {
		LOG_ERROR << "tls cert load failed: " << e.what() << " cert=" << _cert_file << " key=" << _key_file;
		return false;
	}

	std::string subject;
	int days = 0;
	ReadCertInfo(ctx->native_handle(), subject, days);
	{
		std::unique_lock lock(_mutex);
		_context = std::move(ctx);
		_signature = sig;
		_expires_in_days = days;
		_warned_day = days; //启动时会打印一次到期天数，这里先记账，避免一分钟后重复告警
	}

	if (days > 0 && days < 15)
		LOG_WARN << "tls cert expires in " << days << " days, renew now subject=" << subject;
	else
		LOG_INFO << "tls cert ready subject=" << subject << " expires_in_days=" << days;
	return true;
}

void TlsContext::WarnIfExpiring() {
	const int days = ExpiresInDays();
	if (days <= 0 || days >= 15)
		return;
	{
		std::unique_lock lock(_mutex);
		if (_warned_day == days)
			return;
		_warned_day = days;
	}
	LOG_WARN << "tls cert expires in " << days << " days, renew now or clients will fail to connect";
}

void TlsContext::ReloadIfChanged() {
	const std::string sig = Signature();
	{
		std::shared_lock lock(_mutex);
		//文件没换，或换上的这份已经证明读不出来：都不必重试
		if (sig == _signature || sig == _failed_signature)
			return;
	}

	spdlog::info("tls cert file changed, reloading");
	if (!Build()) {
		std::unique_lock lock(_mutex);
		_failed_signature = sig;
		return;
	}
	spdlog::info("tls cert reloaded, new connections use the new certificate");
}

void TlsContext::StartWatch(net::io_context& ioc, unsigned interval_seconds) {
	if (interval_seconds == 0) {
		spdlog::info("tls cert watch disabled");
		return;
	}
	auto timer = std::make_shared<net::steady_timer>(ioc);
	WatchTick(shared_from_this(), timer, interval_seconds);
}
