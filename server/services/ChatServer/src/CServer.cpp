#include "CServer.h"
#include <iostream>
#include "IOContextPool.h"
#include "UserMgr.h"
#include "ConfigMgr.h"
#include "StatusGrpcClient.h"
#include "const.h"

CServer::CServer(boost::asio::io_context& io_context, short port) : _io_context(io_context),
    _port(port), _acceptor(io_context, tcp::endpoint(tcp::v4(), port)),
    _timer(_io_context, std::chrono::seconds(HEART_BEAT_INTERVAL))
{
	auto& cfg = ConfigMgr::Inst();
	_self_name = cfg["SelfServer"]["Name"];
	//对外地址必须是客户端与状态服务都能访问的地址，0.0.0.0 只适合监听不适合上报
	_advertise_host = cfg["SelfServer"]["AdvertiseHost"];
	if (_advertise_host.empty())
		_advertise_host = cfg["SelfServer"]["Host"];
	if (_advertise_host.empty() || _advertise_host == "0.0.0.0")
		_advertise_host = "127.0.0.1";
	//控制面地址单独配置：容器里客户端地址是公网 IP，实例之间仍走回环或服务名
	_advertise_rpc_host = cfg["SelfServer"]["AdvertiseRpcHost"];
	if (_advertise_rpc_host.empty())
		_advertise_rpc_host = _advertise_host;
	_client_port = _port;
	_rpc_port = atoi(cfg["SelfServer"]["RPCPort"].c_str());
	_capacity = atoi(cfg["SelfServer"]["Capacity"].c_str());
	if (_capacity <= 0)
		_capacity = 1000;

	std::cout << "Server start success, listen on port : " << _port << std::endl;
	StartAccept(); //开始接受连接
}

CServer::~CServer() {
	Stop();
	std::cout << "Server destruct listen on port : " << _port << std::endl;
}

void CServer::HandleAccept(std::shared_ptr<CSession> new_session, const boost::system::error_code& error)
{
	//如果没有错误, 则启动新的会话, 否则输出错误信息
	if (!error) {
        //先把连接加入_sessions，再启动它的异步读取，否则读取回调可能先执行并被CheckValid判为无效
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _sessions.emplace(new_session->GetSessionId(), new_session);
        }
        new_session->Start();
	}
	else {
		std::cout << "session accept failed, error is " << error.what() << std::endl;
	}

	if (!_stopped.load())
		StartAccept(); //继续接受下一个连接
}

void CServer::StartAccept()
{
	if (_stopped.load())
	    return;
	//从IOContext池中获取一个IOContext对象, 并创建一个新的会话对象来接受连接
	auto& io_context = IOContextPool::GetInstance()->GetIOContext();
	std::shared_ptr<CSession> new_session = std::make_shared<CSession>(io_context, this);

	//异步接受连接, 当有新的连接到来时, 会调用HandleAccept函数
	_acceptor.async_accept(new_session->GetSocket(),
		std::bind(&CServer::HandleAccept, this, new_session, std::placeholders::_1)); 
}

void CServer::ClearSession(const std::string& session_id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto iter = _sessions.find(session_id);
    if (iter != _sessions.end()) {
        auto uid = iter->second->GetUserId();

        //按uid和session_id一起解绑，只有映射仍指向当前连接时才移除，避免误删同一用户的新连接
        UserMgr::GetInstance()->RmvUserSession(uid, session_id);
        _sessions.erase(iter); //只移除本地会话登记，socket关闭与租约清理由调用方处理
    }
}

bool CServer::CheckValid(const std::string& uuid)
{
    std::lock_guard<std::mutex> lock(_mutex); //保护_sessions的互斥锁
    return _sessions.find(uuid) != _sessions.end();
}

std::shared_ptr<CSession> CServer::GetSession(const std::string& uuid)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _sessions.find(uuid);
    if (it != _sessions.end()) {
        return it->second;
    }
    return nullptr;
}

bool CServer::RegisterSelf()
{
	if (_self_name.empty() || _rpc_port <= 0) {
		std::cout << "[instance] register skipped: SelfServer.Name 或 RPCPort 未配置" << std::endl;
		return false;
	}

	auto reply = StatusGrpcClient::GetInstance()->RegisterInstance(_self_name, _advertise_host,
		_advertise_rpc_host, _client_port, _rpc_port, _capacity);
	if (reply.error() != ErrorCodes::Success) {
		std::cout << "[instance] register failed name=" << _self_name
			<< " err=" << reply.error() << std::endl;
		return false;
	}

	_registered = true;
	_draining = reply.draining();
	std::cout << "[instance] register ok name=" << _self_name << " "
		<< _advertise_host << ":" << _client_port << " rpc=" << _rpc_port
		<< " capacity=" << _capacity << std::endl;
	return true;
}

void CServer::RenewLeases(const std::vector<std::shared_ptr<CSession>>& sessions,
	int conn_count, int session_count)
{
	//注册失效时先补注册；失败就跳过本轮，不因状态服务不可用而断开任何连接
	if (!_registered && !RegisterSelf())
		return;

	std::vector<message::SessionLeaseRef> leases;
	//按 session_id 而不是 uid 建索引：同实例可能同时存在旧连接与新连接，
	//按 uid 匹配会把"旧连接被顶替"的结果错误地应用在新连接上
	std::map<std::string, std::shared_ptr<CSession>> owners;

	auto flush = [&]() {
		//即使没有已认证会话也要发一次：这个请求同时承担实例心跳，
		//少了它实例注册会在 90 秒后过期，新登录直接没有可选实例
		auto reply = StatusGrpcClient::GetInstance()->RenewSessions(_self_name, leases,
			conn_count, session_count);
		if (reply.error() == ErrorCodes::InstanceUnknown) {
			//实例注册过期：下一轮重新注册，已有会话保持不动
			_registered = false;
			std::cout << "[instance] registration expired name=" << _self_name << std::endl;
		}
		else if (reply.error() != ErrorCodes::Success) {
			//连续失败只记日志：租约 TTL 到期由服务端判定离线，客户端不因此掉线
			if (++_renew_failures == 3)
				std::cout << "[lease] renew failing inst=" << _self_name
					<< " err=" << reply.error() << std::endl;
		}
		else {
			_renew_failures = 0;
			_draining = reply.draining();
			for (const auto& result : reply.items()) {
				auto iter = owners.find(result.session_id());
				if (iter == owners.end())
					continue;
				auto session = iter->second;
				if (result.status() == 2) {
					//已被更高 epoch 顶替：先发下线原因，再延迟断开，保证通知写得出去。
					//同时立刻摘掉本地 uid 映射：排队中的请求不应再在一条已失效的连接上执行
					session->NotifyOffline(result.uid(), 1);
					session->CloseSoon(300);
					ClearSession(session->GetSessionId());
					std::cout << "[lease] fenced uid=" << result.uid()
						<< " local_epoch=" << session->GetEpoch() << std::endl;
				}
				else if (result.status() == 1 && result.epoch() > 0) {
					//租约过期后重新授予，本地跟上新 epoch，后续释放才不会被当成旧连接
					session->SetEpoch(result.epoch());
				}
			}
		}

		leases.clear();
		owners.clear();
	};

	for (const auto& session : sessions) {
		const int uid = session->GetUserId();
		const long long epoch = session->GetEpoch();
		if (uid <= 0 || epoch <= 0 || session->IsClosed())
			continue;

		auto& ref = leases.emplace_back();
		ref.set_uid(uid);
		ref.set_session_id(session->GetSessionId());
		ref.set_epoch(epoch);
		owners[session->GetSessionId()] = session;

		if (leases.size() >= LEASE_RENEW_BATCH)
			flush();
	}
	flush();
}

void CServer::on_timer(const boost::system::error_code& ec) {
	if (_stopped.load())
	    return;
    if (ec) {
        std::cout << "timer error: " << ec.message() << std::endl;
        return;
    }
    std::vector<std::shared_ptr<CSession>> expired_sessions;
    std::vector<std::shared_ptr<CSession>> alive_sessions;
    //持锁复制会话表后再遍历快照，避免检测心跳和续租时长时间占用会话表的锁
    std::map<std::string, std::shared_ptr<CSession>> sessions_copy;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        sessions_copy = _sessions;
    }

    time_t now = std::time(nullptr);
    for (auto iter = sessions_copy.begin(); iter != sessions_copy.end(); iter++) {
        auto b_expired = iter->second->IsHeartbeatExpired(now);
        if (b_expired) {
            //标记关闭并投递socket关闭操作，未完成的读取也可能随后报错，清理入口需要能处理重复调用
            iter->second->Close();
            //收集过期信息
            expired_sessions.push_back(iter->second);
            continue;
        }
        alive_sessions.push_back(iter->second);
    }

    //续租与实例心跳合并上报：连接数含未登录连接，会话数只统计已认证连接
    int session_count = 0;
    for (const auto& session : alive_sessions) {
        if (session->GetUserId() > 0 && session->GetEpoch() > 0 && !session->IsClosed())
            ++session_count;
    }
    RenewLeases(alive_sessions, static_cast<int>(alive_sessions.size()), session_count);

    //在会话表锁之外清理过期连接，DealExceptionSession内部还会调用ClearSession获取同一把锁
    for (auto &session : expired_sessions) {
        session->DealExceptionSession();
    }

    //再次设置，下一个60s检测
    _timer.expires_after(std::chrono::seconds(HEART_BEAT_INTERVAL));
    if (!_stopped.load()) {
        _timer.async_wait([this](boost::system::error_code ec) {
            on_timer(ec);
        });
    }
}

void CServer::StartTimer()
{
    //先登记本实例再开始接受登录：状态服务只把新连接分配给已注册的实例
    RegisterSelf();
    auto self(shared_from_this());
    _timer.async_wait([self](boost::system::error_code ec) {
        self->on_timer(ec);
        });
}

void CServer::StopTimer()
{
	_timer.cancel();
}

void CServer::Stop()
{
    //幂等停服：停止接入、定时器并清理所有会话
	if (_stopped.exchange(true))
	    return;

	boost::system::error_code ec; //用于捕获错误码，避免异常抛出
	_acceptor.cancel(ec); //取消所有挂起的异步接受操作
	_acceptor.close(ec); //关闭acceptor，释放端口
	_timer.cancel(); //取消定时器，停止心跳检测

	std::vector<std::shared_ptr<CSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		for (auto& entry : _sessions)
		    sessions.push_back(entry.second); //复制会话指针，避免在清理过程中持锁
	}

	for (auto& session : sessions) {
		session->Close(); //标记关闭并投递socket关闭操作，未完成的读取也可能随后报错，清理入口需要能处理重复调用
		session->DealExceptionSession(); //清理会话表与租约，内部会获取会话表锁
	}

	//主动下线：立即从可选列表移除，避免停机期间还有新客户端被分配过来
	if (_registered && !_self_name.empty()) {
		StatusGrpcClient::GetInstance()->UnregisterInstance(_self_name);
		_registered = false;
	}
}
