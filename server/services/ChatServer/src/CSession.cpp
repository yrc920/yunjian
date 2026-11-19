#include "CSession.h"
#include "CServer.h"
#include <iostream>
#include <sstream>
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include "LogicSystem.h"
#include "RedisMgr.h"
#include "StatusGrpcClient.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include "Log.h"

CSession::CSession(boost::asio::io_context& io_context, CServer* server, std::shared_ptr<TlsContext> tls) :
	_socket(io_context, std::move(tls)), _user_uid(0), _server(server), _b_close(false)
{
	boost::uuids::uuid a_uuid = boost::uuids::random_generator()(); //生成一个随机的UUID
	_session_id = boost::uuids::to_string(a_uuid); //将UUID转换为字符串形式
	_recv_head_node = std::make_shared<MsgNode>(HEAD_TOTAL_LEN); //分配头部节点内存
    _last_heartbeat = std::time(nullptr); //初始化上次接受数据的时间为当前时间
}

CSession::~CSession() {
}

tcp::socket& CSession::GetSocket() {
	return _socket.LowestSocket(); //返回socket对象的引用，以便服务器可以使用它进行通信
}

std::string& CSession::GetSessionId() {
	return _session_id;
}

void CSession::SetUserId(int uid)
{
	_user_uid = uid;
}

int CSession::GetUserId()
{
	return _user_uid;
}

void CSession::SetEpoch(long long epoch)
{
	_epoch = epoch;
}

long long CSession::GetEpoch() const
{
	return _epoch;
}

void CSession::Close() {
    //读失败、写失败和心跳超时都可能关闭同一连接，exchange返回旧值，保证只投递一次关闭操作
    if (_b_close.exchange(true)) {
        return;
    }
    //先标记关闭，让其他线程停止投递新消息，再把实际关闭动作交给socket所属的IO线程
    //每个io_context只有一个工作线程，关闭与异步收发的发起操作在同一线程执行，避免并发操作socket
    auto self = shared_from_this();
    boost::asio::post(_socket.GetExecutor(), [self]() {
        self->_socket.Close();
    });
}

void CSession::CloseSoon(int delay_ms)
{
    //定时器由回调持有，连接提前销毁也不会留下悬空对象
    auto self = shared_from_this();
    auto timer = std::make_shared<boost::asio::steady_timer>(_socket.GetExecutor());
    timer->expires_after(std::chrono::milliseconds(delay_ms));
    timer->async_wait([self, timer](const boost::system::error_code& ec) {
        if (!ec)
            self->Close();
        });
}

std::shared_ptr<CSession>CSession::SharedSelf() {
	//返回共享当前对象所有权的指针，异步回调持有它期间，会话对象不会被销毁
	return shared_from_this();
}

void CSession::Start() {
    //accept在服务器线程完成，会话的读取由自己的IO线程启动；捕获self保证任务执行前会话不会被销毁
    auto self = shared_from_this();
    boost::asio::post(_socket.GetExecutor(), [self]() {
        if (self->IsClosed()) return; //任务排队期间可能已经收到关闭请求
        //TLS 连接先握手再读第一帧；明文连接这里会立即回调成功，后面只有一条路径
        self->_socket.AsyncHandshake([self](const boost::system::error_code& ec) {
            if (ec) {
                //扫描器、证书不匹配或握手超时都会走到这里，属于常见噪声
                if (ec != boost::asio::error::operation_aborted)
                    LOG_DEBUG << "tls handshake failed: " << ec.message();
                //握手失败还没开始读，读回调不会再来，这里直接把会话摘掉，别等心跳超时
                self->Close();
                self->DealExceptionSession();
                return;
            }
            if (!self->IsClosed())
                self->AsyncReadHead(HEAD_TOTAL_LEN);
            });
    });
}

void CSession::Send(std::string msg, std::uint16_t msgid)
{
    if (IsClosed()) return; //已关闭的连接不再接受发送任务
    if (msg.empty()) return; //空响应没有意义，直接丢弃，不能因此断开连接
    //长度字段是 2 字节；超过上限说明调用方拼了一个过大的响应。
    //此时回一个最小错误包让客户端结束等待，而不是关连接——关连接会连带影响该用户的其他会话。
    if (msg.size() > MAX_RESPONSE_LEN) {
        LOG_WARN << "response too large, size=" << msg.size()
            << " msgid=" << msgid;
        static const std::string fallback =
            "{\"error\":" + std::to_string(static_cast<int>(ErrorCodes::ResponseTooLarge))
            + ",\"message\":\"response too large\"}";
        Send(fallback, msgid); //fallback 很小，不会再次进入本分支
        return;
    }
    //pending_sends同时统计等待进入IO线程和已经进入发送队列的消息，不能只检查_send_que
    //如果IO线程正忙，只限制_send_que仍会让大量发送任务积压在io_context中
    if (_pending_sends.fetch_add(1) >= MAX_SENDQUE) {
        _pending_sends.fetch_sub(1); //本次消息不入队，归还刚占用的计数
        Close();
        DealExceptionSession();
        return;
    }
    auto self = shared_from_this();
    //消息内容移入回调，由回调持有到真正入队，调用方返回后不会留下失效的字符串引用
    boost::asio::post(_socket.GetExecutor(), [self, msg = std::move(msg), msgid]() mutable {
        self->SendOnIO(std::move(msg), msgid);
    });
}

void CSession::Send(const char* msg, std::size_t max_length, std::uint16_t msgid)
{
    if (!msg || max_length == 0) return;
    Send(std::string(msg, max_length), msgid); //复制指定字节数，复用统一的长度检查和投递流程
}

void CSession::SendOnIO(std::string msg, std::uint16_t msgid)
{
    //发送任务排队后连接仍可能被关闭，这种消息尚未进入_send_que，只归还自身占用的计数
    if (IsClosed()) {
        _pending_sends.fetch_sub(1);
        return;
    }
    const bool writing = !_send_que.empty(); //非空表示已有一条异步写负责推进队列
    _send_que.push(std::make_shared<SendNode>(msg.data(), msg.size(), msgid));
    //已有写操作时只入队，后续由HandleWrite续写，避免同时对同一个socket发起多次async_write
    if (writing) return;
    auto& node = _send_que.front();
    _socket.AsyncWrite(boost::asio::buffer(node->_data, node->_total_len),
        std::bind(&CSession::HandleWrite, this, std::placeholders::_1, SharedSelf()));
}

void CSession::HandleWrite(const boost::system::error_code& error,
	std::shared_ptr<CSession> shared_self)
{
	try {
		//如果发生错误，输出错误信息
		if (error) {
			LOG_DEBUG << "handle write failed, error is " << error.what();
			Close(); //关闭连接
            DealExceptionSession(); //处理异常连接
            //async_write完成前必须保留队首缓冲区，因此不能在Close中直接清空发送队列
            //现在写回调已返回结果，可以释放队列中的消息；尚未入队的任务由SendOnIO各自归还计数
            std::queue<std::shared_ptr<SendNode>> empty;
            _pending_sends.fetch_sub(_send_que.size());
            _send_que.swap(empty);
            return;
        }

		_send_que.pop(); //从发送队列中移除已经发送的消息节点
        _pending_sends.fetch_sub(1); //当前消息发送完成，释放一个发送名额

		//如果发送队列中还有消息在等待发送，则继续发送下一个消息
		if (!_send_que.empty() && !IsClosed()) {
			auto& msgnode = _send_que.front(); //获取发送队列中的下一个消息节点
			//异步发送消息数据，当发送完成后会调用HandleWrite函数进行处理
			_socket.AsyncWrite(boost::asio::buffer(msgnode->_data, msgnode->_total_len),
				std::bind(&CSession::HandleWrite, this, std::placeholders::_1, shared_self));
		}
	}
	catch (std::exception& e) {
		LOG_ERROR << "Exception code : " << e.what();
	}
}

void CSession::AsyncReadBody(int total_len)
{
	auto self = shared_from_this();
	//异步读取消息体数据，当读取完成后会调用回调函数进行处理
	asyncReadFull(total_len, [self, this, total_len](
		const boost::system::error_code& ec, std::size_t bytes_transfered) {
			try {
				//如果发生错误，关闭连接并清除会话
				if (ec) {
					//客户端正常断开（EOF / RST）不记日志：3000 人在线时每次重连都打一行会淹没有效信息
					if (ec != boost::asio::error::eof && ec != boost::asio::error::connection_reset)
						LOG_DEBUG << "handle read failed: " << ec.message()
							<< " (" << ec.value() << ")";
					Close(); //关闭连接
					DealExceptionSession(); //处理异常连接
					return;
				}

				//如果读取的字节数小于消息长度，说明数据不完整，关闭连接并清除会话
				//封装的asyncReadFull函数会保证读取到指定长度的数据，除非发生错误，这里的判断其实是冗余的
				if (bytes_transfered < static_cast<std::size_t>(total_len)) {
					LOG_DEBUG << "read length not match, read [" << bytes_transfered << "] , total ["
						<< total_len << "]";
					Close(); //关闭连接
                DealExceptionSession(); //关闭并清理异常连接
					return;
				}

			    //判断连接无效
                if (!_server->CheckValid(_session_id)) {
                    Close();
                    return;
                }

				memcpy(_recv_msg_node->_data, _data, bytes_transfered); //将读取到的数据复制到消息节点内
				_recv_msg_node->_cur_len += bytes_transfered; //更新消息节点的当前长度
				//在消息数据末尾添加字符串结束符，确保消息数据可以被正确解析为字符串
				_recv_msg_node->_data[_recv_msg_node->_total_len] = '\0';
				//不记录原始消息体，登录请求包含 token。

                UpdateHeartbeat(); //任意完整请求都会刷新活跃时间，不限于专门的心跳消息
				//此处将消息投递到逻辑队列中
				LogicSystem::GetInstance()->PostMsgToQue(
					std::make_shared<LogicNode>(shared_from_this(), _recv_msg_node));
				
				AsyncReadHead(HEAD_TOTAL_LEN); //继续监听头部接受事件
			}
			catch (std::exception& e) {
				LOG_ERROR << "Exception code is " << e.what();
			}
		});
}

void CSession::AsyncReadHead(int /*total_len*/)
{
	auto self = shared_from_this();
	//异步读取头部数据，当读取完成后会调用回调函数进行处理
	asyncReadFull(HEAD_TOTAL_LEN, [self, this](const boost::system::error_code& ec,
		std::size_t bytes_transfered) {
		try {
			//如果发生错误，关闭连接并清除会话
			if (ec) {
				//客户端正常断开（EOF / RST）不记日志：3000 人在线时每次重连都打一行会淹没有效信息
				if (ec != boost::asio::error::eof && ec != boost::asio::error::connection_reset
					&& ec != ssl::error::stream_truncated)
					LOG_DEBUG << "handle read failed: " << ec.message()
						<< " (" << ec.value() << ")";
				Close(); //关闭连接
				DealExceptionSession(); //处理异常连接
				return;
			}

			//如果读取的字节数小于头部长度，说明数据不完整，关闭连接并清除会话
			//封装的asyncReadFull函数会保证读取到指定长度的数据，除非发生错误，这里的判断其实是冗余的
			if (bytes_transfered < HEAD_TOTAL_LEN) {
				LOG_DEBUG << "read length not match, read [" << bytes_transfered << "] , total ["
					<< HEAD_TOTAL_LEN << "]";
				Close(); //关闭连接
                DealExceptionSession(); //关闭并清理异常连接
				return;
			}

		    //判断连接无效
            if (!_server->CheckValid(_session_id)) {
                Close();
                return;
            }

			_recv_head_node->Clear(); //清空头部节点数据
			memcpy(_recv_head_node->_data, _data, bytes_transfered); //将读取到的数据复制到头部节点内

			//获取头部MSGID数据
			std::uint16_t msg_id = 0;
			memcpy(&msg_id, _recv_head_node->_data, HEAD_ID_LEN); //从头部数据中提取消息ID
			//网络字节序转化为本地字节序
			msg_id = boost::asio::detail::socket_ops::network_to_host_short(msg_id);
            //这里先限制消息ID的数值范围，具体ID是否注册了处理函数由LogicSystem判断
			if (msg_id == 0 || msg_id > MAX_LENGTH) {
				LOG_WARN << "invalid msg_id is " << msg_id;
				Close();
                DealExceptionSession(); //关闭并清理异常连接
				return;
			}

			//获取消息长度数据
			std::uint16_t msg_len = 0;
			//从头部数据中提取消息长度
			memcpy(&msg_len, _recv_head_node->_data + HEAD_ID_LEN, HEAD_DATA_LEN);
			//网络字节序转化为本地字节序
			msg_len = boost::asio::detail::socket_ops::network_to_host_short(msg_len);
            //用无符号数读取包长，避免高位为1时被当成负数而绕过上限检查
            //拒绝空包和超过接收缓冲区的消息体，检查通过后才分配RecvNode并启动读取
			if (msg_len == 0 || msg_len > MAX_LENGTH) {
				LOG_WARN << "invalid data length is " << msg_len;
				Close();
                DealExceptionSession(); //关闭并清理异常连接
				return;
			}

			//分配消息节点内存，并设置消息长度和消息ID
			_recv_msg_node = std::make_shared<RecvNode>(msg_len, msg_id); 
			AsyncReadBody(msg_len); //继续异步读取消息体，准备接收数据
		}
		catch (std::exception& e) {
			LOG_ERROR << "Exception code is " << e.what();
		}
	});
}

void CSession::NotifyOffline(int uid, int reason)
{
    Json::Value rtvalue; //创建一个Json对象，用于存储返回的结果
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["uid"] = uid;
    rtvalue["reason"] = reason; //客户端按原因选择提示语，不能一律显示"网络断开"

    std::string return_str = rtvalue.toStyledString(); //将Json对象转换为字符串形式
    Send(return_str, ID_NOTIFY_OFF_LINE_REQ); //调用Send函数发送离线通知消息给客户端
    return;
}

//图片已经完整落在资源服务器上，接收方收到后先查本地缓存，没有就发起断点下载
void CSession::NotifyImageRecv(long long message_id, int from_uid, int to_uid,
    long long thread_id, const std::string& name, const std::string& client_msg_id,
    long long total_size, long long created_at)
{
    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["message_id"] = Json::Int64(message_id);
    rtvalue["thread_id"] = Json::Int64(thread_id);
    rtvalue["fromuid"] = from_uid;
    rtvalue["touid"] = to_uid;
    rtvalue["msgid"] = client_msg_id; //与历史同步同一个 ID，客户端据此去重
    rtvalue["name"] = name;
    rtvalue["total_size"] = Json::Int64(total_size);
    rtvalue["time"] = Json::Int64(created_at); //落库时间，接收方时间条与列表排序都用它
    Send(rtvalue.toStyledString(), ID_NOTIFY_IMG_CHAT_MSG_REQ);
}

//视频/文件的通知与图片共用消息标识，额外带类型和落库时的元数据；
//接收方据此先画卡片和视频首帧，真正下载要等用户点击
void CSession::NotifyResourceRecv(long long message_id, int from_uid, int to_uid,
    long long thread_id, const std::string& name, const std::string& client_msg_id,
    long long total_size, int msg_type, const std::string& content, long long created_at)
{
    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    rtvalue["message_id"] = Json::Int64(message_id);
    rtvalue["thread_id"] = Json::Int64(thread_id);
    rtvalue["fromuid"] = from_uid;
    rtvalue["touid"] = to_uid;
    rtvalue["msgid"] = client_msg_id; //与历史同步同一个 ID，客户端据此去重
    rtvalue["name"] = name;
    rtvalue["total_size"] = Json::Int64(total_size);
    rtvalue["msg_type"] = msg_type;
    rtvalue["content"] = content;
    rtvalue["time"] = Json::Int64(created_at); //落库时间，接收方时间条与列表排序都用它
    Send(rtvalue.toStyledString(), ID_NOTIFY_RESOURCE_CHAT_MSG_REQ);
}

//读取完整长度
void CSession::asyncReadFull(std::size_t maxLength,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	::memset(_data, 0, MAX_LENGTH); //清空接收缓冲区
	asyncReadLen(0, maxLength, handler); //调用asyncReadLen函数保证读取到指定长度的数据
}

//读取指定字节数
void CSession::asyncReadLen(std::size_t read_len, std::size_t total_len,
	std::function<void(const boost::system::error_code&, std::size_t)> handler)
{
	auto self = shared_from_this();
	//异步读取数据到接收缓冲区中
	_socket.AsyncReadSome(boost::asio::buffer(_data + read_len, total_len - read_len),
		[read_len, total_len, handler, self](
			const boost::system::error_code& ec, std::size_t  bytesTransfered) {
			//如果发生错误
			if (ec) {
				//客户端正常断开（EOF / RST）不记日志：3000 人在线时每次重连都打一行会淹没有效信息
				if (ec != boost::asio::error::eof && ec != boost::asio::error::connection_reset
					&& ec != ssl::error::stream_truncated)
					LOG_DEBUG << "handle read failed: " << ec.message()
						<< " (" << ec.value() << ")";
				handler(ec, read_len + bytesTransfered); //调用回调函数，传入错误码和实际读取的长度
				return;
			}

			//如果读取的字节数加上已经读取的长度大于等于总长度，说明已经读取到完整的数据
			if (read_len + bytesTransfered >= total_len) {
				handler(ec, read_len + bytesTransfered); //调用回调函数，传入错误码和实际读取的长度
				return;
			}

			//没有错误，且长度不足则继续读取
			self->asyncReadLen(read_len + bytesTransfered, total_len, handler);
		});
}

bool CSession::IsHeartbeatExpired(std::time_t& now) {
    double diff_sec = std::difftime(now, _last_heartbeat);
    if (diff_sec > HEART_THRESHOLD) {
        LOG_DEBUG << "heartbeat expired, session id is  " << _session_id;
        return true;
    }
    return false;
}

void CSession::UpdateHeartbeat()
{
    time_t now = std::time(nullptr);
    _last_heartbeat = now;
}

bool CSession::AllowMessage()
{
    //令牌桶按单调时钟补充：20 条/秒、突发 50。
    //超限只拒绝这一条并回 1105，不断连接——刷屏的客户端不该影响同账号的其他窗口
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(_bucket_mutex);
    const double elapsed = std::chrono::duration<double>(now - _bucket_time).count();
    _bucket_tokens = std::min(MESSAGE_BURST, _bucket_tokens + elapsed * MESSAGE_RATE_PER_SECOND);
    _bucket_time = now;
    if (_bucket_tokens < 1.0)
        return false;
    _bucket_tokens -= 1.0;
    return true;
}

void CSession::DealExceptionSession()
{
    //登录登记与断连清理共用会话锁，保证读取 uid 与 epoch 时登录流程已结束
    auto lifecycle = LockLifecycle();
    if (_cleanup_started.exchange(true)) return; //多条异常路径只允许执行一次清理
    auto self = shared_from_this();
    const int uid = GetUserId();

    //先摘掉本地登记：即使状态服务不可达，本机也不会再把消息投给这条连接
    Defer defer([self, this]() {
        _server->ClearSession(_session_id);
        });

    //未认证连接没有租约，清理本地记录即可
    if (uid == 0)
        return;

    //释放租约走异步队列，断连路径不等待状态服务；
    //已被顶替时服务端按 epoch 拒绝，不会删掉新会话的租约
    StatusGrpcClient::GetInstance()->ReleaseAsync(uid, _session_id, GetEpoch());
}


LogicNode::LogicNode(std::shared_ptr<CSession> session, std::shared_ptr<RecvNode> recvnode)
	: _session(session), _recvnode(recvnode) {

}
