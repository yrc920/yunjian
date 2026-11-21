#include "LogicSystem.h"
#include "UserCache.h"
#include "StatusGrpcClient.h"
#include "MysqlDao.h"
#include "const.h"
#include "JsonUtil.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "ChatGrpcClient.h"
#include "SessionRouter.h"
#include "ConfigMgr.h"
#include "CServer.h"
#include "RequestValidation.h"
#include <charconv>
#include <cstdint>
#include <ctime>
#include <map>
#include "Log.h"

LogicSystem::LogicSystem() :_b_stop(false), _p_server(nullptr), _delivery_stop(false)
{
	RegisterCallBacks(); //注册消息处理函数
	//每个分片一个逻辑线程：数据库慢查询只影响落到该分片的会话，
	//其他分片的心跳、登录与消息照常处理
	for (std::size_t shard = 0; shard < kLogicShards; ++shard)
		_workers.emplace_back(&LogicSystem::DealMsg, this, shard);
	_delivery_thread = std::thread(&LogicSystem::DeliveryLoop, this);
}

LogicSystem::~LogicSystem()
{
    Stop();
}

std::pair<std::size_t, std::size_t> LogicSystem::QueueDepth()
{
	std::size_t logic_queued = 0;
	for (std::size_t shard = 0; shard < kLogicShards; ++shard) {
		std::lock_guard<std::mutex> lock(_mutexes[shard]);
		logic_queued += _msg_ques[shard].size();
	}

	std::size_t delivery_queued = 0;
	{
		std::lock_guard<std::mutex> lock(_delivery_mutex);
		delivery_queued = _delivery_queue.size();
	}
	return { logic_queued, delivery_queued };
}

void LogicSystem::Stop()
{
    //停服标志是原子的：各分片在各自锁内检查，不会错过 notify
    _b_stop.store(true);
	for (auto& consume : _consumes)
		consume.notify_all(); //唤醒所有分片
	for (auto& worker : _workers) {
		if (worker.joinable())
			worker.join(); //每个分片把队列里剩余消息处理完再退出
	}

	//业务线程退出后不会再有新任务，再停投递线程。
	//未投递的任务直接丢弃，消息已经在库里，接收方下次登录会同步补回；
	//如果在这里排空队列，停服时最多要等"任务数 × RPC 截止时间"
	{
		std::lock_guard<std::mutex> lock(_delivery_mutex);
		_delivery_stop = true;
		_delivery_queue.clear();
	}
	_delivery_cv.notify_one();
	if (_delivery_thread.joinable()) _delivery_thread.join();

    _p_server.reset(); //业务线程退出后释放服务器引用，避免服务器存活时间超过它依赖的主io_context
}

void LogicSystem::PostDeliveryTask(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(_delivery_mutex);
		if (_delivery_stop)
			return; //停服后不再排队
		if (_delivery_queue.size() >= kDeliveryQueueMax) {
			//对端长时间不可达时只丢实时投递，消息仍在库里，登录同步会补回
			LOG_WARN << "[delivery] queue full, drop one task";
			return;
		}
		_delivery_queue.push_back(std::move(task));
	}
	_delivery_cv.notify_one();
}

void LogicSystem::DeliveryLoop()
{
	for (;;) {
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(_delivery_mutex);
			_delivery_cv.wait(lock, [this] {
				return _delivery_stop || !_delivery_queue.empty();
			});
			if (_delivery_queue.empty())
				return; //只有停服且队列已空才退出
			task = std::move(_delivery_queue.front());
			_delivery_queue.pop_front();
		}
		try {
			task(); //单条投递失败只记日志，不影响后续任务
		}
		catch (const std::exception& error) {
			LOG_ERROR << "[delivery] task failed: " << error.what();
		}
	}
}

void LogicSystem::PostMsgToQue(std::shared_ptr<LogicNode> msg)
{
	//按会话哈希选分片：同一连接的消息固定落到同一线程，顺序不变；
	//不同连接可以并行，慢查询不会让所有用户一起排队
	const std::size_t shard = std::hash<std::string>{}(msg->_session->GetSessionId()) % kLogicShards;
	std::unique_lock<std::mutex> unique_lk(_mutexes[shard]);

	if (_b_stop.load())
		return; //停服后不再接收新消息

	//队列满说明业务线程已经跟不上。不能静默丢弃：客户端会一直停在"发送中"，
	//这里按请求 ID + 1 回一个明确错误，让用户知道这条没被处理
	if (_msg_ques[shard].size() >= MAX_LOGICQUE) {
		const auto id = msg->_recvnode->_msg_id;
		auto session = msg->_session;
		unique_lk.unlock(); //回包不占队列锁
		LOG_WARN << "[logic] queue full, reject msg id=" << id << " shard=" << shard;
		Json::Value response;
		response["error"] = ErrorCodes::ServiceUnavailable;
		session->Send(JsonToCompact(response), ResponseId(id));
		return;
	}

	_msg_ques[shard].push(msg);
	if (_msg_ques[shard].size() == 1) {
		unique_lk.unlock();
		_consumes[shard].notify_one(); //只有从空变非空才需要唤醒
	}
}

void LogicSystem::SetServer(std::shared_ptr<CServer> pserver) {
    //主线程在开始处理accept事件前登记服务器，业务线程之后才能检查连接是否仍然有效
    _p_server = pserver;
}

void LogicSystem::DealMsg(std::size_t shard) {
    for (;;) {
        std::shared_ptr<LogicNode> node;
        {
            std::unique_lock<std::mutex> lock(_mutexes[shard]);
            //条件变量等待时释放队列锁，收到通知后重新检查条件，也能处理没有实际消息的虚假唤醒
            _consumes[shard].wait(lock, [this, shard] {
                return _b_stop.load() || !_msg_ques[shard].empty();
            });
            if (_msg_ques[shard].empty()) return; //停服且本分片已空
            node = _msg_ques[shard].front();
            _msg_ques[shard].pop();
        }
        //离开作用域后释放队列锁，再执行可能阻塞的数据库或RPC调用，避免IO线程也跟着等待
        auto session = node->_session;
        const auto id = node->_recvnode->_msg_id;
        auto handler = _fun_callbacks.find(id);
        //消息排队期间连接可能断开或被踢下线，不能只依赖接收消息时的连接状态
        if (handler == _fun_callbacks.end() || session->IsClosed() ||
            !_p_server || !_p_server->CheckValid(session->GetSessionId())) continue;

        //刷屏保护只针对三类消息写入请求；心跳、已读、拉取不受限，
        //超限回 1105 而不是断开连接，客户端可以降速重试
        if ((id == ID_TEXT_CHAT_MSG_REQ || id == ID_IMG_CHAT_MSG_REQ
            || id == ID_RESOURCE_CHAT_MSG_REQ) && !session->AllowMessage()) {
            Json::Value busy;
            busy["error"] = ErrorCodes::TooManyRequests;
            session->Send(JsonToCompact(busy), ResponseId(id));
            continue;
        }
        try {
            std::string data(node->_recvnode->_data, node->_recvnode->_cur_len);
            Json::Value root;
            //先检查JSON语法，再统一检查字段类型、登录状态和操作者uid，handler只处理通过校验的请求
            auto error = JsonFromString(data, root)
                ? ValidateChatRequest(id, root, session->GetUserId()) : Error_Json;
            //会话中有uid并不代表它还是当前登录连接，还要确认UserMgr中的uid确实指向这条会话
            //这样被新连接替代的会话即使还有排队消息，也不能继续执行好友或聊天操作
            if (error == Success && id != MSG_CHAT_LOGIN &&
                UserMgr::GetInstance()->GetSession(session->GetUserId()) != session) {
                error = TokenInvalid;
            }
            if (error != Success) {
                Json::Value response;
                response["error"] = error;
                session->Send(JsonToCompact(response), ResponseId(id));
                continue;
            }
            handler->second(session, id, root);
        } catch (const std::exception& error) {
            //异常在单条消息的边界处理，关闭对应连接后继续消费其他消息，避免异常逃出线程导致进程终止
            LOG_ERROR << "Chat request failed: " << error.what();
            session->Close();
            session->DealExceptionSession();
        }
    }
}

void LogicSystem::RegisterCallBacks()
{
	//注册消息处理函数
	_fun_callbacks[MSG_CHAT_LOGIN] = std::bind(&LogicSystem::LoginHandler, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	//注册搜索用户信息处理函数
	_fun_callbacks[ID_SEARCH_USER_REQ] = std::bind(&LogicSystem::SearchInfo, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	//注册申请添加好友处理函数
	_fun_callbacks[ID_ADD_FRIEND_REQ] = std::bind(&LogicSystem::AddFriendApply, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	_fun_callbacks[ID_AUTH_FRIEND_REQ] = std::bind(&LogicSystem::AuthFriendApply, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	_fun_callbacks[ID_TEXT_CHAT_MSG_REQ] = std::bind(&LogicSystem::DealChatTextMsg, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_IMG_CHAT_MSG_REQ] = std::bind(&LogicSystem::DealChatImgMsg, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_RESOURCE_CHAT_MSG_REQ] = std::bind(&LogicSystem::DealResourceChatMsg, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_CREATE_PRIVATE_CHAT_REQ] = std::bind(&LogicSystem::CreatePrivateChat, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_LOAD_CHAT_THREAD_REQ] = std::bind(&LogicSystem::LoadChatThreads, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_LOAD_CHAT_MSG_REQ] = std::bind(&LogicSystem::LoadChatMessages, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

    _fun_callbacks[ID_HEART_BEAT_REQ] = std::bind(&LogicSystem::HeartBeatHandler, this,
        std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	_fun_callbacks[ID_UPDATE_PROFILE_REQ] = std::bind(&LogicSystem::UpdateProfile, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_UPDATE_FRIEND_REMARK_REQ] = std::bind(&LogicSystem::UpdateFriendRemark, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_GET_PROFILE_REQ] = std::bind(&LogicSystem::GetProfile, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);

	//已读上报与撤回：两条都会改变对方看到的状态，需要转发通知
	_fun_callbacks[ID_MARK_READ_REQ] = std::bind(&LogicSystem::MarkMessagesRead, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_RECALL_MSG_REQ] = std::bind(&LogicSystem::RecallMessage, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_DELETE_FRIEND_REQ] = std::bind(&LogicSystem::DeleteFriend, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	//群聊：建群、群资料、群设置与成员管理
	_fun_callbacks[ID_CREATE_GROUP_REQ] = std::bind(&LogicSystem::CreateGroup, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_GET_GROUP_INFO_REQ] = std::bind(&LogicSystem::GetGroupInfo, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_UPDATE_GROUP_REQ] = std::bind(&LogicSystem::UpdateGroup, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_GROUP_MEMBER_OP_REQ] = std::bind(&LogicSystem::GroupMemberOp, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
	_fun_callbacks[ID_LOGOUT_REQ] = std::bind(&LogicSystem::LogoutHandler, this,
		std::placeholders::_1, std::placeholders::_2, std::placeholders::_3);
}

Json::Value LogicSystem::BuildThreadItem(const ChatThreadInfo& thread) const
{
	Json::Value item;
	item["thread_id"] = Json::Int64(thread.thread_id);
	item["thread_type"] = thread.thread_type;
	item["time"] = Json::Int64(thread.updated_at);
	item["unread"] = thread.unread;
	if (thread.thread_type == static_cast<int>(ThreadType::Group)) {
		item["group_name"] = thread.group_name;
		item["icon"] = thread.group_icon;
		item["icon_owner"] = thread.group_icon_owner;
		item["owner"] = thread.group_owner;
		item["member_count"] = thread.group_member_count;
		item["my_role"] = thread.my_role;
		item["muted"] = thread.muted;
		item["at_me"] = thread.at_me;
	}
	else {
		item["uid"] = thread.peer_uid;
		item["name"] = thread.peer_name;
		item["nick"] = thread.peer_nick;
		item["icon"] = thread.peer_icon;
		item["sex"] = thread.peer_sex;
	}
	return item;
}

int LogicSystem::ThreadTypeOf(long long thread_id)
{
	//会话类型不会变，走进程内缓存；未命中才查库，失败按单聊继续
	return MysqlDao::GetInstance()->GetThreadTypeCached(thread_id);
}

//一条已落库消息的公共字段：文本、图片、视频文件与系统消息共用同一套键名，
//客户端的历史解析与实时通知因此可以走同一个入口
Json::Value LogicSystem::BuildMessageItem(const ChatMessageInfo& message, int status)
{
	Json::Value item;
	item["msgid"] = message.client_msg_id;
	item["content"] = message.content;
	item["thread_id"] = Json::Int64(message.thread_id);
	item["message_id"] = Json::Int64(message.message_id);
	item["fromuid"] = message.sender_id;
	item["touid"] = message.recv_id;
	item["msg_type"] = message.msg_type;
	item["status"] = status;
	item["time"] = Json::Int64(message.created_at);
	item["mentions"] = message.mentions;
	item["recalled"] = message.recalled;
	return item;
}

//建群：创建者成为群主，勾选的好友一次性写入成员表；成功后给其他成员推 5009 created，
//他们收到后把群加进会话列表并拉一次历史，首屏就能看到"创建了群聊"这条系统消息
