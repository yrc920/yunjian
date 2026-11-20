#pragma once
// 用于管理每个连接, 处理请求, 发送响应
//
// 明文与 HTTPS 共用同一套 HTTP 逻辑，只在传输层分叉：构造时给了 TlsContext
// 就先做 TLS 握手，没给就直接读请求。

#include "const.h"
#include "TlsContext.h"

class HttpConnection : public std::enable_shared_from_this<HttpConnection>
{
	//使LogicSystem可以访问HttpConnection的私有成员, 以便在处理请求时设置响应内容
	friend class LogicSystem;
public:
	//tls 为空表示明文 HTTP；非空表示这条连接先握手再读请求
	HttpConnection(boost::asio::io_context& ioc, std::shared_ptr<TlsContext> tls);
	void Start(); //开始处理连接

	//acceptor 接受连接用。握手前是裸 socket，握手后是 ssl_stream 内部的同一个 socket，
	//远端地址与执行器都还从这里取，所以 LogicSystem 不需要区分两种连接
	tcp::socket& GetSocket() { return _tls_stream ? _tls_stream->next_layer() : _socket; }

	//处理函数把回包推迟到哈希线程结果回来之后再发：
	//HandleReq 看到这个标记就不自动写响应
	void DeferResponse();
	//在连接所属的 IO 线程调用：写入包体并发送
	void SendDeferred(const std::string& body);
	//处理函数显式设定状态码（健康检查失败时回 503），HandleReq 不再覆盖
	void SetResult(http::status status);

	//请求体与 JSON 回包：入口处理器只依赖这两个方法，
	//不必为了拼响应而成为 HttpConnection 的友元
	std::string RequestBody() const
	{
		return beast::buffers_to_string(_request.body().data());
	}
	void SetJsonResponse(const std::string& body)
	{
		_response.set(http::field::content_type, "application/json");
		beast::ostream(_response.body()) << body;
	}

private:
	void DoRead(); //按连接类型发起异步读
	void DoWrite(); //按连接类型发起异步写
	void WriteResponse(); //补上响应长度并发出去
	void FinishWrite(); //写完收尾：TLS 先发 close_notify，明文直接 shutdown_send
	void Close(); //关掉底层 socket，同时取消超时定时器
	void CheckDeadline(); //检查连接是否超时, 超时则关闭连接
	void HandleReq(); //处理请求
	void PreParseGetParam(); //预处理GET请求的URL, 将URL中的参数解析到_get_params中

	tcp::socket _socket; //裸 socket：负责 accept，TLS 模式下随后被移入 _tls_stream
	std::unique_ptr<ssl::stream<tcp::socket>> _tls_stream; //非空即 HTTPS
	std::shared_ptr<ssl::context> _tls_context; //连接生命周期内不变，证书热加载不影响本连接
	beast::flat_buffer _buffer{ 8192 }; //用于执行读操作的缓冲区
	http::request<http::dynamic_body> _request; //请求消息
	http::response<http::dynamic_body> _response; //响应消息
	//用于在连接处理上设置截止时间的定时器
	net::steady_timer deadline_{
		_socket.get_executor(), std::chrono::seconds(60)
	};
	std::string _get_url; //GET请求的URL路径, 不包含查询字符串
	std::unordered_map<std::string, std::string> _get_params; //GET请求的参数映射, 键为参数名, 值为参数值
	bool _deferred = false; //回包由处理函数自己发（口令哈希在线程池里完成）
	bool _result_set = false; //状态码由处理函数设定
};
