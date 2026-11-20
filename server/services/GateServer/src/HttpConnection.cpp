#include "HttpConnection.h"
#include "LogicSystem.h"
#include <stdexcept>
#include "Log.h"

namespace {
//握手与请求读取的截止时间。默认构造的 steady_timer 永不过期，
//必须显式设置，否则半开连接与慢速请求会一直占着 socket
constexpr int kRequestTimeoutSeconds = 15;
} // namespace

HttpConnection::HttpConnection(boost::asio::io_context& ioc, std::shared_ptr<TlsContext> tls)
	: _socket(ioc), _tls_context(tls ? tls->Get() : nullptr) {} //将io_context绑定到socket中

void HttpConnection::Start() {
	auto self = shared_from_this();
	//先给整条连接一个截止时间：握手、读请求、同步业务处理都算在内
	deadline_.expires_after(std::chrono::seconds(kRequestTimeoutSeconds));
	if (_tls_context) {
		//整条连接持有同一份证书上下文：热加载只影响之后建立的连接
		_tls_stream = std::make_unique<ssl::stream<tcp::socket>>(std::move(_socket), *_tls_context);
		//握手同样受截止时间约束，半开连接不会一直占着 socket
		CheckDeadline();
		_tls_stream->async_handshake(ssl::stream_base::server, [self](beast::error_code ec) {
			if (ec) {
				//扫描器往 443 发明文、客户端不信任证书都会走到这里，属于常见的正常噪声
				LOG_DEBUG << "tls handshake failed: " << ec.message();
				self->Close();
				return;
			}
			self->DoRead();
			});
		return;
	}

	//先登记截止时间检查再开始读取，客户端只发送部分请求时也能触发超时关闭
	//计时回调仍依赖IO线程执行，同步业务处理占用线程时并不能被这个定时器强制中断
	CheckDeadline();
	DoRead();
}

void HttpConnection::DoRead() {
	auto self = shared_from_this();
	//同一个处理体接两种流：出错判断、异常兜底、交给 HandleReq 都不必写两遍
	auto on_read = [self](beast::error_code ec, std::size_t bytes_transferred) {
		try {
			if (ec) {
				//客户端正常关闭（curl/短连接）会报 end_of_stream，不是服务端错误
				if (ec != http::error::end_of_stream && ec != ssl::error::stream_truncated)
					LOG_DEBUG << "http read err: " << ec.message() << " (" << ec.value() << ")";
				return;
			}
			//处理读到的数据
			boost::ignore_unused(bytes_transferred); //已读数据长度直接忽略
			self->HandleReq(); //处理请求
		}
		catch (std::exception& exp) {
			LOG_WARN << "request handling failed: " << exp.what();
			//请求解析或处理抛出异常时返回400，避免只打印日志却让客户端一直等不到响应
			self->_response.result(http::status::bad_request);
			self->_response.set(http::field::content_type, "text/plain");
			beast::ostream(self->_response.body()) << "Bad request";
			self->WriteResponse();
		}
	};

	if (_tls_stream)
		http::async_read(*_tls_stream, _buffer, _request, on_read);
	else
		http::async_read(_socket, _buffer, _request, on_read);
}

//将十进制的char转为16进制
unsigned char ToHex(unsigned char x)
{
	return  x > 9 ? x + 55 : x + 48;
}

//将16进制转为十进制的char
unsigned char FromHex(unsigned char x)
{
	unsigned char y;
	if (x >= 'A' && x <= 'F') y = x - 'A' + 10;
	else if (x >= 'a' && x <= 'f') y = x - 'a' + 10;
	else if (x >= '0' && x <= '9') y = x - '0';
    else throw std::invalid_argument("Invalid URL escape"); //非十六进制字符交给请求入口按错误请求处理
	return y;
}

//URL编码函数, 将字符串中的特殊字符转换为%加两位十六进制数的形式
std::string UrlEncode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//判断是否仅有数字和字母构成
		if (isalnum((unsigned char)str[i]) ||
			(str[i] == '-') ||
			(str[i] == '_') ||
			(str[i] == '.') ||
			(str[i] == '~'))
			strTemp += str[i];
		else if (str[i] == ' ') //为空字符
			strTemp += "+";
		else
		{
			//其他字符需要提前加%并且高四位和低四位分别转为16进制
			strTemp += '%';
			strTemp += ToHex((unsigned char)str[i] >> 4);
			strTemp += ToHex((unsigned char)str[i] & 0x0F);
		}
	}
	return strTemp;
}

//URL解码函数, 将%加两位十六进制数的形式转换回原来的字符, 同时将+转换为空格
std::string UrlDecode(const std::string& str)
{
	std::string strTemp = "";
	size_t length = str.length();
	for (size_t i = 0; i < length; i++)
	{
		//还原+为空
		if (str[i] == '+') strTemp += ' ';
		//遇到%将后面的两个字符从16进制转为char再拼接
		else if (str[i] == '%')
		{
            //百分号后必须有两个字符，先检查剩余长度，再递增下标读取，避免访问字符串末尾之外
            if (i + 2 >= length) throw std::invalid_argument("Incomplete URL escape");
			unsigned char high = FromHex((unsigned char)str[++i]);
			unsigned char low = FromHex((unsigned char)str[++i]);
			strTemp += high * 16 + low;
		}
		else strTemp += str[i];
	}
	return strTemp;
}

void HttpConnection::PreParseGetParam() {
	//提取 URI
	auto uri = _request.target();
	//查找查询字符串的开始位置（即 '?' 的位置）
	auto query_pos = uri.find('?');
	//如果没有查询字符串，直接返回
	if (query_pos == std::string::npos) {
		_get_url = uri;
		return;
	}

	_get_url = uri.substr(0, query_pos); //提取 URL 路径部分, 不包含查询字符串
	std::string query_string = uri.substr(query_pos + 1); //提取查询字符串部分
	std::string key; 
	std::string value;
	size_t pos = 0;
	//循环处理查询字符串中的每个参数对, 以 '&' 分隔
	while ((pos = query_string.find('&')) != std::string::npos) {
		auto pair = query_string.substr(0, pos); //提取一个参数对
		size_t eq_pos = pair.find('='); //查找参数对中 '=' 的位置, 用于分隔键和值
		//如果找到了 '=', 则将 '=' 前的部分作为键, '=' 后的部分作为值, 并进行URL解码后存储到_get_params中
		if (eq_pos != std::string::npos) {
			key = UrlDecode(pair.substr(0, eq_pos));
			value = UrlDecode(pair.substr(eq_pos + 1));
			_get_params[key] = value;
		}
		query_string.erase(0, pos + 1); //从查询字符串中删除已处理的参数对, 继续处理下一个参数对
	}
	//处理最后一个参数对（如果没有 & 分隔符）
	if (!query_string.empty()) {
		size_t eq_pos = query_string.find('=');	//查找参数对中 '=' 的位置, 用于分隔键和值
		//如果找到了 '=', 则将 '=' 前的部分作为键, '=' 后的部分作为值, 并进行URL解码后存储到_get_params中
		if (eq_pos != std::string::npos) {
			key = UrlDecode(query_string.substr(0, eq_pos));
			value = UrlDecode(query_string.substr(eq_pos + 1));
			_get_params[key] = value;
		}
	}
}

void HttpConnection::HandleReq() {
	//设置响应的HTTP版本和连接选项
	_response.version(_request.version());
	_response.keep_alive(false);

	//如果是GET方法
	if (_request.method() == http::verb::get) {
		PreParseGetParam(); //预处理GET请求的URL, 将URL中的参数解析到_get_params中
		//调用LogicSystem处理GET请求, 传入纯路径URL
		bool success = LogicSystem::GetInstance()->HandleGet(_get_url, shared_from_this());
		//如果LogicSystem没有处理这个URL, 则返回404错误
		if (!success) {
			_response.result(http::status::not_found); //设置响应状态码为404 Not Found
			_response.set(http::field::content_type, "text/plain"); //设置响应内容类型为纯文本
			beast::ostream(_response.body()) << "url not found\r\n"; //设置响应内容为错误信息
			WriteResponse(); //发送响应
			return;
		}
		if (!_result_set)
			_response.result(http::status::ok); //处理函数没指定时默认 200
		_response.set(http::field::server, "GateServer"); //设置响应头中的服务器信息
		WriteResponse(); //发送响应
		return;
	}

	//如果是POST方法
	if (_request.method() == http::verb::post) {
		//调用LogicSystem处理POST请求, 传入URL
		bool success = LogicSystem::GetInstance()->HandlePost(_request.target(), shared_from_this());
		//如果LogicSystem没有处理这个URL, 则返回404错误
		if (!success) {
			_response.result(http::status::not_found); //设置响应状态码为404 Not Found
			_response.set(http::field::content_type, "text/plain"); //设置响应内容类型为纯文本
			beast::ostream(_response.body()) << "url not found\r\n"; //设置响应内容为错误信息
			WriteResponse(); //发送响应
			return;
		}

		//口令哈希在线程池里执行，处理函数稍后自己调 SendDeferred 发响应
		if (_deferred)
			return;

		if (!_result_set)
			_response.result(http::status::ok); //处理函数没指定时默认 200
		_response.set(http::field::server, "GateServer"); //设置响应头中的服务器信息
		WriteResponse(); //发送响应
		return;
	}

	//GET/POST 之外的方法不能挂着不返回，否则客户端只能等到截止时间
	_response.result(http::status::method_not_allowed);
	_response.set(http::field::content_type, "text/plain");
	beast::ostream(_response.body()) << "method not allowed\r\n";
	WriteResponse();
}

void HttpConnection::SetResult(http::status status) {
	_result_set = true;
	_response.result(status);
}

void HttpConnection::DeferResponse() {
	_deferred = true;
	_response.result(http::status::ok);
	_response.set(http::field::server, "GateServer");
	//异步链路（口令线程池）没有经过 WriteJson，内容类型要在这里补齐
	_response.set(http::field::content_type, "application/json");
}

void HttpConnection::SendDeferred(const std::string& body) {
	beast::ostream(_response.body()) << body;
	WriteResponse();
}

void HttpConnection::WriteResponse() {
	_response.content_length(_response.body().size()); //设置响应内容长度
	DoWrite();
}

void HttpConnection::DoWrite() {
	auto self = shared_from_this();
	auto on_write = [self](beast::error_code ec, std::size_t) {
		if (ec && ec != ssl::error::stream_truncated)
			LOG_DEBUG << "http write err: " << ec.message() << " (" << ec.value() << ")";
		self->FinishWrite();
	};

	if (_tls_stream)
		http::async_write(*_tls_stream, _response, on_write);
	else
		http::async_write(_socket, _response, on_write);
}

void HttpConnection::FinishWrite() {
	auto self = shared_from_this();
	if (_tls_stream) {
		//先发 close_notify 再关，客户端才不会把响应当成被截断；
		//对端不回 close_notify 也无妨，5 秒后由定时器兜底关闭
		deadline_.expires_after(std::chrono::seconds(5));
		_tls_stream->async_shutdown([self](beast::error_code) { self->Close(); });
		return;
	}
	//发送完成后关闭服务器端
	beast::error_code ec;
	self->_socket.shutdown(tcp::socket::shutdown_send, ec);
	self->deadline_.cancel(); //取消超时检查
}

void HttpConnection::Close() {
	beast::error_code ec;
	deadline_.cancel();
	GetSocket().close(ec);
}

void HttpConnection::CheckDeadline() {
	auto self = shared_from_this();
	//异步等待截止时间到达
	deadline_.async_wait([self](beast::error_code ec) {
		if (!ec) {
			//关闭套接字以取消任何未完成的操作
			self->Close();
		}
		});
}
