#include "LogicSystem.h"
#include "UserCache.h"
#include "StatusGrpcClient.h"
#include "MysqlDao.h"
#include "const.h"
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

namespace {
//加好友双方各自的第一句: 申请方留言为空时用默认值, 同意方固定发通过语。
//通过语是申请方唯一能看到的"对方同意了"的信号, 要像正常聊天消息一样可读、可提醒
constexpr const char* kDefaultFriendWelcome = "你好，很高兴认识你";
constexpr const char* kDefaultApproveWelcome = "好友验证通过了，随时可以找我聊天";

//@ 名单在请求里是 uid 数组，落库与未读查询用逗号分隔串。
//去重并忽略非正数，避免客户端传入的脏值拼进 SQL 参数
std::string JoinMentions(const Json::Value& array)
{
	std::string result;
	if (!array.isArray())
		return result;
	for (const auto& item : array) {
		//客户端历史上传过字符串形式的 uid，两种都接受，避免 @ 静默丢失
		int uid = 0;
		if (item.isInt()) {
			uid = item.asInt();
		}
		else if (item.isString()) {
			const auto text = item.asString();
			auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), uid);
			if (error != std::errc{} || end != text.data() + text.size())
				uid = 0;
		}
		if (uid <= 0)
			continue;
		const std::string text = std::to_string(uid);
		if (("," + result + ",").find("," + text + ",") != std::string::npos)
			continue;
		if (!result.empty())
			result += ",";
		result += text;
	}
	return result;
}
}

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
		session->Send(response.toStyledString(), static_cast<std::uint16_t>(id + 1));
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
            session->Send(busy.toStyledString(), static_cast<std::uint16_t>(id + 1));
            continue;
        }
        try {
            std::string data(node->_recvnode->_data, node->_recvnode->_cur_len);
            Json::Value root;
            Json::Reader reader;
            //先检查JSON语法，再统一检查字段类型、登录状态和操作者uid，handler只处理通过校验的请求
            auto error = reader.parse(data, root)
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
                session->Send(response.toStyledString(), id + 1); //已注册的请求，其响应ID均为请求ID加1
                continue;
            }
            handler->second(session, id, data);
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

void LogicSystem::LoginHandler(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader; //创建Json解析器对象
	Json::Value root; //创建Json值对象，用于存储解析后的数据
	reader.parse(msg_data, root); //解析Json字符串，将结果存储在root对象中

	auto uid = root["uid"].asInt(); //从解析后的Json对象中获取用户ID
	auto token = root["token"].asString(); //从解析后的Json对象中获取用户token

	Json::Value rtvalue; //创建Json值对象，用于存储返回给客户端的数据
	Defer defer([this, &rtvalue, session]() {
		//将Json值对象转换为字符串形式，准备发送给客户端
		std::string return_str = rtvalue.toStyledString();
		//通过会话对象发送消息给客户端, 消息ID为用户登陆回包
		session->Send(return_str, MSG_CHAT_LOGIN_RSP);
		});

	//会话锁只包住最后的登记动作（见函数末尾）。
	//令牌校验、资料/好友加载与状态服务的租约申请都不改会话状态，
	//放进锁里会把 9 点登录高峰整体串行化（6 次查询 + 2 秒 RPC）
	if (session->IsClosed()) return; //连接在排队期间可能已经关闭

	//从redis获取用户token是否正确
	std::string uid_str = std::to_string(uid); //将用户ID转换为字符串形式
	std::string token_key = USERTOKENPREFIX + uid_str;
	std::string token_value = "";
	bool success = RedisMgr::GetInstance()->Get(token_key, token_value); //从Redis中获取用户token
	//如果Redis中没有用户token，说明用户ID无效，返回uid无效错误码
	if (!success) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	//如果Redis中的用户token与客户端提供的token不匹配，说明token无效，返回token无效错误码
	if (token_value != token) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return;
	}

	rtvalue["error"] = ErrorCodes::Success;

	std::string base_key = USER_BASE_INFO + uid_str; //构建用户基本信息在Redis中的键名
	auto user_info = std::make_shared<UserInfo>(); //初始化用户信息对象
	bool b_base = usercache::GetBaseInfo(base_key, uid, user_info); //获取用户基本信息
	//如果获取用户基本信息失败，说明用户ID无效，返回uid无效错误码
	if (!b_base) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	//将用户基本信息填充到Json值对象中，准备返回给客户端
	rtvalue["uid"] = uid;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;

	//登录时先带一页会话, 客户端据此建立 uid 与 thread_id 的对应关系;
	//取不到会话不影响登录本身, 好友列表仍然可用。
	//首屏只取 50 条：每行有三个相关子查询，100 条会让登录高峰的数据库开销翻倍；
	//客户端拿到 thread_more 后按游标继续拉
	std::vector<ChatThreadInfo> chat_threads;
	bool thread_more = false;
	if (MysqlDao::GetInstance()->GetChatThreads(uid, 0, 50, chat_threads, thread_more)) {
		Json::Value thread_array(Json::arrayValue);
		for (const auto& thread : chat_threads)
			thread_array.append(BuildThreadItem(thread));
		rtvalue["thread_list"] = thread_array;
		rtvalue["thread_more"] = thread_more; //还有更多会话时客户端用最后一条 thread_id 继续拉
	}

	//从数据库获取申请列表
	std::vector<std::shared_ptr<ApplyInfo>> apply_list;
	auto b_apply = GetFriendApplyInfo(uid, apply_list); //获取好友申请信息列表
	//如果获取好友申请信息列表成功
	if (b_apply) {
		for (auto& apply : apply_list) {
			Json::Value obj;
			obj["name"] = apply->_name;
			obj["uid"] = apply->_uid;
			obj["icon"] = apply->_icon;
			obj["nick"] = apply->_nick;
			obj["sex"] = apply->_sex;
			obj["desc"] = apply->_desc;
			obj["status"] = apply->_status;
			obj["id"] = Json::Int64(apply->_id); //前端按它倒序，新申请排最上面
			obj["direction"] = 0; //别人申请我
			rtvalue["apply_list"].append(obj); //将好友申请信息添加到Json值对象的apply_list数组中
		}
	}

	//我发出去的申请同样下发, 客户端在「新的朋友」里显示等待验证
	std::vector<std::shared_ptr<ApplyInfo>> sent_apply_list;
	if (MysqlDao::GetInstance()->GetSentApplyList(uid, sent_apply_list, 0, 10)) {
		for (auto& apply : sent_apply_list) {
			Json::Value obj;
			obj["name"] = apply->_name;
			obj["uid"] = apply->_uid;
			obj["icon"] = apply->_icon;
			obj["nick"] = apply->_nick;
			obj["sex"] = apply->_sex;
			obj["desc"] = apply->_desc;
			obj["status"] = apply->_status;
			obj["id"] = Json::Int64(apply->_id);
			obj["direction"] = 1; //我申请别人
			rtvalue["apply_list"].append(obj);
		}
	}

	//从数据库获取好友列表
	std::vector<std::shared_ptr<UserInfo>> friend_list;
	bool b_friend_list = GetFriendList(uid, friend_list);
	for (auto& friend_ele : friend_list) {
		Json::Value obj;
		obj["name"] = friend_ele->name;
		obj["uid"] = friend_ele->uid;
		obj["icon"] = friend_ele->icon;
		obj["nick"] = friend_ele->nick;
		obj["sex"] = friend_ele->sex;
		obj["desc"] = friend_ele->desc;
		obj["back"] = friend_ele->back;
		//标签随好友列表一次性下发, 客户端不用再逐个好友查
		obj["tags"] = Json::arrayValue;
		for (const auto& tag : friend_ele->tags)
			obj["tags"].append(tag);
		rtvalue["friend_list"].append(obj); //将好友信息添加到Json值对象的friend_list数组中
	}

	//令牌已校验通过；这里申请租约，由状态服务决定谁是当前会话并返回单调递增的 epoch。
	//申请放在资料与好友列表加载之后：新设备登录失败时不能把旧设备踢掉，用户不会两头落空。
	//同一 uid 的申请与登记串行执行，避免低 epoch 的并发请求事后覆盖 UserMgr 映射
	std::lock_guard<std::mutex> login_guard(LoginLock(uid));
	auto server_name = ConfigMgr::Inst().GetValue("SelfServer", "Name");
	auto lease = StatusGrpcClient::GetInstance()->AllocateSession(
		uid, token, session->GetSessionId(), server_name);
	if (lease.error() != ErrorCodes::Success) {
		//状态服务不可用时按失败处理：客户端可以重试，已有会话不受影响
		rtvalue["error"] = lease.error();
		return;
	}

	//登记阶段才拿会话锁：与断连清理互斥，保证 uid 与 epoch 要么一起可见，要么都不写。
	//锁在函数返回时先于回包 Defer 析构，发送失败触发的清理不会重复获取自身持有的锁
	auto lifecycle = session->LockLifecycle();

	//申请期间连接可能已经关闭，必须把刚拿到的租约还回去，否则要等 TTL 到期才释放
	if (session->IsClosed() || !_p_server->CheckValid(session->GetSessionId())) {
		StatusGrpcClient::GetInstance()->ReleaseAsync(uid, session->GetSessionId(), lease.epoch());
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		return;
	}

	session->SetEpoch(lease.epoch());
	session->SetUserId(uid); //session绑定用户uid
	UserMgr::GetInstance()->SetUserSession(uid, session); //本机映射由这里维护，不再写 Redis
	//本实例的路由缓存可能刚存过一个"离线"结论，登录成功后立刻失效，
	//否则同实例的发送方在缓存过期前仍会把消息当成离线处理
	SessionRouter::Inst().Invalidate(uid);
	spdlog::info("chat login uid={} sid={} epoch={} inst={}", uid,
		session->GetSessionId(), lease.epoch(), ConfigMgr::Inst()["SelfServer"]["Name"]);

	if (!lease.kicked_instance().empty()) {
		LOG_INFO << "[login] uid=" << uid << " epoch=" << lease.epoch()
			<< " superseded inst=" << lease.kicked_instance();
	}
	return;
}

void LogicSystem::SearchInfo(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader; //创建Json解析器对象
	Json::Value root; //创建Json值对象，用于存储解析后的数据
	reader.parse(msg_data, root); //解析Json字符串，将结果存储在root对象中
	auto uid_str = root["uid"].asString(); //从解析后的Json对象中获取用户ID字符串

	Json::Value rtvalue; //创建Json值对象，用于存储返回给客户端的数据
	Defer defer([this, &rtvalue, session]() {
		//将Json值对象转换为字符串形式，准备发送给客户端
		std::string return_str = rtvalue.toStyledString();
		//通过会话对象发送消息给客户端, 消息ID为搜索用户回包
		session->Send(return_str, ID_SEARCH_USER_RSP);
		});

	bool b_digit = isPureDigit(uid_str); //判断用户ID字符串是否纯数字
	if (b_digit) {
		GetUserByUid(uid_str, rtvalue); //如果用户ID字符串是纯数字，则按照用户ID查询用户信息
	}
	else {
		GetUserByName(uid_str, rtvalue); //如果用户ID字符串不是纯数字，则按照用户名查询用户信息
	}

	//关系状态由服务端给出：客户端本地缓存里的"好友"可能早已被对方删除，
	//只有库里两行都在才算好友，否则搜索页会一直显示"发消息"而无法重新申请
	const int target = rtvalue.get("uid", 0).asInt();
	const int me = session->GetUserId();
	if (rtvalue.get("error", ErrorCodes::Error_Json).asInt() == ErrorCodes::Success
		&& target > 0 && me > 0) {
		rtvalue["relation"] = target == me ? -1
			: MysqlDao::GetInstance()->GetFriendRelation(me, target);
	}
	return;
}

void LogicSystem::AddFriendApply(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader; //创建Json解析器对象
	Json::Value root; //创建Json值对象，用于存储解析后的数据
	reader.parse(msg_data, root); //解析Json字符串，将结果存储在root对象中
	//从解析后的Json对象中获取申请添加好友的相关信息
	auto uid = root["uid"].asInt();
	std::string welcome = root["applyname"].asString();
	if (welcome.empty())
		welcome = kDefaultFriendWelcome;
	auto bakname = root["bakname"].asString();
	auto touid = root["touid"].asInt();
	//标签缺省时是空数组; 申请通过后这些标签直接成为申请方的分组
	std::vector<std::string> tags;
	for (const auto& tag : root["tags"])
		tags.push_back(tag.asString());

	Json::Value rtvalue; //创建Json值对象，用于存储返回给客户端的数据
	rtvalue["error"] = ErrorCodes::Success; //默认返回成功状态码
	Defer defer([this, &rtvalue, session]() {
		//将Json值对象转换为字符串形式，准备发送给客户端
		std::string return_str = rtvalue.toStyledString();
		//通过会话对象发送消息给客户端, 消息ID为申请添加好友回包
		session->Send(return_str, ID_ADD_FRIEND_RSP);
		});

    if (!MysqlDao::GetInstance()->AddFriendApply(uid, touid, welcome, bakname, tags)) {
        rtvalue["error"] = ErrorCodes::RPCFailed;
        return;
    }

	//目标用户未在线时不推送：申请已经写入数据库，对方登录后能在申请列表里看到
	std::string to_ip_value;
	if (!SessionRouter::Inst().Lookup(touid, to_ip_value))
		return;

	//从配置文件中获取自身服务器的名称
	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];

	std::string base_key = USER_BASE_INFO + std::to_string(uid);
	auto apply_info = std::make_shared<UserInfo>(); //初始化申请人用户信息对象
	bool b_info = usercache::GetBaseInfo(base_key, uid, apply_info); //获取申请人用户基本信息

	//如果目标用户在当前服务器上
	if (to_ip_value == self_name) {
		auto session = UserMgr::GetInstance()->GetSession(touid); //从UserMgr中获取目标用户的会话对象
		//如果目标用户的会话对象存在，说明目标用户在线，直接发送通知给对方
		if (session) {
			//在内存中则直接发送通知对方
			Json::Value notify; //创建Json值对象，用于存储通知信息
			notify["error"] = ErrorCodes::Success;
			notify["applyuid"] = uid;
			notify["name"] = b_info ? apply_info->name : std::to_string(uid);
			notify["desc"] = welcome;
			if (b_info) {
				notify["icon"] = apply_info->icon;
				notify["sex"] = apply_info->sex;
				notify["nick"] = apply_info->nick;
			}
			std::string return_str = notify.toStyledString();
			//通过会话对象发送消息给目标用户, 消息ID为通知添加好友请求
			session->Send(return_str, ID_NOTIFY_ADD_FRIEND_REQ);
		}
		return;
	}

	AddFriendReq add_req; //创建AddFriendReq消息对象，准备发送通知
	add_req.set_applyuid(uid);
	add_req.set_touid(touid);
	add_req.set_name(b_info ? apply_info->name : std::to_string(uid));
	add_req.set_desc(welcome);
	//如果获取申请人用户基本信息成功，则将申请人用户的头像、性别和昵称填充到AddFriendReq消息对象中
	if (b_info) {
		add_req.set_icon(apply_info->icon);
		add_req.set_sex(apply_info->sex);
		add_req.set_nick(apply_info->nick);
	}

	//通过ChatGrpcClient发送通知添加好友请求给目标用户所在的服务器
	ChatGrpcClient::GetInstance()->NotifyAddFriend(to_ip_value, add_req);
}

void LogicSystem::AuthFriendApply(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);

	auto uid = root["fromuid"].asInt();
	auto touid = root["touid"].asInt();
	auto back_name = root["back"].asString();

	Json::Value  rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	auto user_info = std::make_shared<UserInfo>();

	std::string base_key = USER_BASE_INFO + std::to_string(touid);
	bool b_info = usercache::GetBaseInfo(base_key, touid, user_info);
	if (b_info) {
		rtvalue["name"] = user_info->name;
		rtvalue["nick"] = user_info->nick;
		rtvalue["icon"] = user_info->icon;
		rtvalue["sex"] = user_info->sex;
		rtvalue["uid"] = touid;
	}
	else {
		rtvalue["error"] = ErrorCodes::UidInvalid;
	}


	Defer defer([this, &rtvalue, session]() {
		std::string return_str = rtvalue.toStyledString();
		session->Send(return_str, ID_AUTH_FRIEND_RSP);
		});

    if (!b_info) return;
    //DAO在同一事务中检查申请、确认状态并建立双向好友关系，只有提交成功后才通知对方
    std::string apply_desc;
    std::string peer_back; //申请方当初给通过方设的备注，回传给申请方本地显示
    std::vector<std::string> peer_tags; //申请方当初选的分组，同样回传
    //同意弹层可以直接选分组：自己这一侧的好友关系里存下来，不必等通过后再补
    std::vector<std::string> my_tags;
    for (const auto& tag : root["tags"]) {
        if (tag.isString() && !tag.asString().empty())
            my_tags.push_back(tag.asString());
    }
	bool first_approve = false;
	if (!MysqlDao::GetInstance()->AddFriend(uid, touid, back_name, my_tags, apply_desc,
        peer_back, peer_tags, first_approve)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
	//同意方自己填的备注随回包返回，客户端不必等下一次登录才看到
	rtvalue["back"] = back_name;
	//好友关系已经提交, 会话和欢迎消息属于附加内容; 写失败只记日志, 不回滚也不让审批失败
	long long thread_id = MysqlDao::GetInstance()->GetOrCreatePrivateChat(uid, touid);
	if (thread_id > 0)
		rtvalue["thread_id"] = Json::Int64(thread_id);
	else
		LOG_ERROR << "create private chat failed, friend relation kept: " << uid << " - " << touid;

	//重复同意（例如连点两次）只回成功：关系与欢迎语都已经写过，再写会造出重复消息
	if (!first_approve)
		return;

	std::vector<ChatMessageInfo> applicant_msgs;
	std::vector<ChatMessageInfo> approver_msgs;
	if (thread_id > 0) {
		//申请方没填留言时用默认句兜底, 同意方不填写, 固定发通过语
		std::string applicant_welcome = apply_desc;
		if (applicant_welcome.empty())
			applicant_welcome = kDefaultFriendWelcome;
		//client_msg_id 带上通过时间：删好友后重新通过会复用同一对 uid，
		//固定 ID 会被唯一键挡掉，申请方就再也收不到新的通过语
		const std::string stamp = std::to_string(std::time(nullptr));
		const std::string applicant_id = "friend-welcome-" + std::to_string(touid) + "-"
			+ std::to_string(uid) + "-" + stamp;
		const std::string approver_id = "friend-welcome-" + std::to_string(uid) + "-"
			+ std::to_string(touid) + "-" + stamp;
		//先写申请方的留言，再写同意方的默认句，message_id 顺序就是聊天里的显示顺序
		if (!MysqlDao::GetInstance()->SaveChatMessages(touid, uid, thread_id,
			{{applicant_id, applicant_welcome}}, applicant_msgs))
			LOG_ERROR << "save applicant welcome failed: " << applicant_id;
		if (!MysqlDao::GetInstance()->SaveChatMessages(uid, touid, thread_id,
			{{approver_id, kDefaultApproveWelcome}}, approver_msgs))
			LOG_ERROR << "save approver welcome failed: " << approver_id;
	}

	//同一条欢迎消息对两个客户端的方向不同: 自己发的算已发送, 对方发的算未读
	auto append_welcome = [&](Json::Value& array, const ChatMessageInfo& message, int status) {
		Json::Value item;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["message_id"] = Json::Int64(message.message_id);
		item["thread_id"] = Json::Int64(thread_id);
		item["status"] = status;
		item["fromuid"] = message.sender_id;
		item["touid"] = message.recv_id;
		item["time"] = Json::Int64(message.created_at); //欢迎消息也参与会话排序
		array.append(item);
	};
	//同意方视角：申请方的留言是收到的未读，自己的默认句是已发送
	Json::Value approver_view(Json::arrayValue);
	for (const auto &message : applicant_msgs)
		append_welcome(approver_view, message, 3);
	for (const auto &message : approver_msgs)
		append_welcome(approver_view, message, 1);
	if (approver_view.size() > 0)
		rtvalue["welcome_msgs"] = approver_view;

	//申请方视角：自己的留言是已发送，同意方的默认句是收到的未读
	Json::Value applicant_view(Json::arrayValue);
	for (const auto &message : applicant_msgs)
		append_welcome(applicant_view, message, 1);
	for (const auto &message : approver_msgs)
		append_welcome(applicant_view, message, 3);

	//通过好友的关系已经落库，对方不在线时跳过推送，登录后拉取即可
	std::string to_ip_value;
	if (!SessionRouter::Inst().Lookup(touid, to_ip_value))
		return;

	auto& cfg = ConfigMgr::Inst();
	auto self_name = cfg["SelfServer"]["Name"];
	//直接通知对方有认证通过消息
	if (to_ip_value == self_name) {
		auto session = UserMgr::GetInstance()->GetSession(touid);
		if (session) {
			//在内存中则直接发送通知对方
			Json::Value  notify;
			notify["error"] = ErrorCodes::Success;
			notify["fromuid"] = uid;
			notify["touid"] = touid;
			if (thread_id > 0)
				notify["thread_id"] = Json::Int64(thread_id);
			notify["welcome_msgs"] = applicant_view;
			notify["back"] = peer_back;
			notify["tags"] = Json::arrayValue;
			for (const auto& tag : peer_tags)
				notify["tags"].append(tag);
			std::string base_key = USER_BASE_INFO + std::to_string(uid);
			auto user_info = std::make_shared<UserInfo>();
			bool b_info = usercache::GetBaseInfo(base_key, uid, user_info);
			if (b_info) {
				notify["name"] = user_info->name;
				notify["nick"] = user_info->nick;
				notify["icon"] = user_info->icon;
				notify["sex"] = user_info->sex;
			}
			else {
				notify["error"] = ErrorCodes::UidInvalid;
			}


			std::string return_str = notify.toStyledString();
			session->Send(return_str, ID_NOTIFY_AUTH_FRIEND_REQ);
		}
		return;
	}

	AuthFriendReq auth_req;
	auth_req.set_fromuid(uid);
	auth_req.set_touid(touid);
	auth_req.set_back(peer_back);
	for (const auto& tag : peer_tags)
		auth_req.add_tags(tag);
	if (thread_id > 0)
		auth_req.set_thread_id(thread_id);
	for (const auto &item : applicant_view) {
		auto *welcome = auth_req.add_welcome_msgs();
		welcome->set_msgid(item["msgid"].asString());
		welcome->set_msgcontent(item["content"].asString());
		welcome->set_message_id(item["message_id"].asInt64());
		welcome->set_status(item["status"].asInt());
		welcome->set_fromuid(item["fromuid"].asInt());
		welcome->set_touid(item["touid"].asInt());
	}

	//发送通知
	ChatGrpcClient::GetInstance()->NotifyAuthFriend(to_ip_value, auth_req);
}

void LogicSystem::DealChatTextMsg(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);

	auto uid = root["fromuid"].asInt();
	auto touid = root["touid"].asInt();
	long long thread_id = root.get("thread_id", Json::Int64(0)).asInt64();

	const Json::Value  arrays = root["text_array"];

	//先把回执按"全部失败"填好, 后面写库成功后逐条改成已发送;
	//中途任何一步返回, Defer 都会把当前状态发回客户端
	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["text_array"] = Json::arrayValue;
	rtvalue["fromuid"] = uid;
	rtvalue["touid"] = touid;
	for (const auto &item : arrays) {
		Json::Value response_item;
		response_item["msgid"] = item["msgid"].asString();
		response_item["content"] = item["content"].asString();
		response_item["status"] = 2;
		rtvalue["text_array"].append(response_item);
	}
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_TEXT_CHAT_MSG_RSP);
	});
	std::vector<std::pair<std::string, std::string>> contents;
	for (const auto& item : arrays) contents.emplace_back(item["msgid"].asString(), item["content"].asString());
	//会话类型以库里为准：同一个文本通道既能写单聊消息，也能写群消息。
	//thread_id 为 0 时一定是单聊，客户端还没建会话
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	//先落库再投递: 对方离线时消息已经保存在 chat_message, 登录后还能拉回
	std::vector<ChatMessageInfo> saved;
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		if (!MysqlDao::GetInstance()->SaveGroupMessages(uid, thread_id, mentions, contents, saved)) {
			rtvalue["error"] = ErrorCodes::RPCFailed;
			rtvalue["group_invalid"] = true; //不在群里或群已解散
			return;
		}
	}
	else if (!MysqlDao::GetInstance()->SaveChatMessages(uid, touid, thread_id, contents, saved)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		//写库失败最常见的原因是关系不完整（任何一方删了好友），
		//单独标出来，客户端才能提示"你们已不是好友"而不是笼统的发送失败
		if (!MysqlDao::GetInstance()->IsMutualFriend(uid, touid))
			rtvalue["not_friend"] = true;
		return;
	}
	//thread_id 可能是0(客户端还没建会话), 以写库结果为准回填
	if (!saved.empty()) thread_id = saved.front().thread_id;
	spdlog::debug("message saved thread={} sender={} count={} last_id={}", thread_id, uid,
		saved.size(), saved.empty() ? 0 : saved.back().message_id);
	if (session->IsClosed() || !_p_server->CheckValid(session->GetSessionId())) return;
	rtvalue["thread_id"] = Json::Int64(thread_id);
	rtvalue["thread_type"] = thread_type;
	rtvalue["text_array"] = Json::arrayValue;
	Json::Value notify = rtvalue;
	for (const auto& message : saved) {
		Json::Value item;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["thread_id"] = Json::Int64(message.thread_id);
		item["message_id"] = Json::Int64(message.message_id);
		item["status"] = 1;
		item["time"] = Json::Int64(message.created_at); //发送方按服务端时间显示
		item["mentions"] = message.mentions;
		rtvalue["text_array"].append(item);
	}
	notify["text_array"] = rtvalue["text_array"];
	//同一批消息发给接收方时状态改成未读, 与发送方看到的已发送区分
	for (auto &item : notify["text_array"])
		item["status"] = 3;

	//群聊：除自己外全部成员都要收到，本实例直投、其他实例聚合后转发
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		std::vector<int> members;
		if (!MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members))
			return;
		BroadcastGroupEvent(thread_id, members, uid, notify, ID_NOTIFY_TEXT_CHAT_MSG_REQ);
		return;
	}

	//消息已经提交到数据库；实时投递交给投递线程。
	//对端实例不可达时 gRPC 会阻塞到截止时间，不能让唯一的业务线程陪着等
	TextChatMsgReq text_msg_req;
	text_msg_req.set_fromuid(uid);
	text_msg_req.set_touid(touid);
	text_msg_req.set_thread_id(thread_id);
	for (const auto& message : saved) {
		auto* text_msg = text_msg_req.add_textmsgs();
		text_msg->set_msgid(message.client_msg_id);
		text_msg->set_msgcontent(message.content);
		text_msg->set_message_id(message.message_id);
		text_msg->set_status(3);
		text_msg->set_fromuid(uid);
		text_msg->set_touid(touid);
		text_msg->set_time(message.created_at); //跨实例也按落库时间显示，不用转发时刻
	}

	PostDeliveryTask([req = std::move(text_msg_req), notify = std::move(notify),
		touid]() mutable {
		std::string to_ip_value;
		if (!SessionRouter::Inst().Lookup(touid, to_ip_value))
			return; //对方不在线：消息已在库里，登录同步会补发

		//对方在本实例: 直接投递, 不再走一次 gRPC
		if (to_ip_value == ConfigMgr::Inst()["SelfServer"]["Name"]) {
			auto peer_session = UserMgr::GetInstance()->GetSession(touid);
			if (peer_session)
				peer_session->Send(notify.toStyledString(), ID_NOTIFY_TEXT_CHAT_MSG_REQ);
			return;
		}

		auto forwarded = ChatGrpcClient::GetInstance()->NotifyTextChatMsg(
			to_ip_value, req, Json::Value());
		if (forwarded.error() == ErrorCodes::UidInvalid) {
			//缓存的位置过期：对方已经换实例，失效后回源一次再投，不能直接当离线
			SessionRouter::Inst().Invalidate(touid);
			std::string retry_ip;
			if (SessionRouter::Inst().Lookup(touid, retry_ip) && retry_ip != to_ip_value) {
				ChatGrpcClient::GetInstance()->NotifyTextChatMsg(
					retry_ip, req, Json::Value());
			}
		}
		//投递失败只影响"立刻看到"，接收方下次登录仍会用 1006 同步补回
	});
}

void LogicSystem::DealChatImgMsg(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	const int uid = root["fromuid"].asInt();
	const int touid = root["touid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string md5 = root["md5"].asString();
	const std::string name = root["name"].asString();
	const std::string unique_id = root["unique_id"].asString();

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["fromuid"] = uid;
	response["touid"] = touid;
	response["md5"] = md5;
	response["name"] = name;
	response["unique_id"] = unique_id;

	//消息先落库, content存资源文件名; 资源本体由客户端随后上传到 ResourceServer。
	//群聊按会话类型写群消息表，通知留到资源上传完成后统一广播
	ChatMessageInfo saved;
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	const bool group = thread_type == static_cast<int>(ThreadType::Group);
	const bool saved_ok = group
		? MysqlDao::GetInstance()->SaveGroupResourceMessage(uid, thread_id, mentions, unique_id,
			1, name, saved)
		: MysqlDao::GetInstance()->SaveResourceMessage(uid, touid, thread_id, unique_id, 1,
			name, saved);
	if (!saved_ok) {
		response["error"] = ErrorCodes::RPCFailed;
		response["group_invalid"] = group;
		session->Send(response.toStyledString(), ID_IMG_CHAT_MSG_RSP);
		return;
	}

	response["thread_id"] = Json::Int64(saved.thread_id);
	response["thread_type"] = thread_type;
	response["message_id"] = Json::Int64(saved.message_id);
	response["status"] = 0; //资源尚未上传; 接收方通知留到资源完成后的章节
	response["time"] = Json::Int64(saved.created_at);
	session->Send(response.toStyledString(), ID_IMG_CHAT_MSG_RSP);
}

//视频/文件消息只落库，资源本体由客户端收到回执后上传；视频首帧是独立资源，
//一起写进 content，接收方下载前就能用它做预览
void LogicSystem::DealResourceChatMsg(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	const int uid = root["fromuid"].asInt();
	const int touid = root["touid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	const int msg_type = root["msg_type"].asInt();
	const std::string md5 = root["md5"].asString();
	const std::string name = root["name"].asString();
	const std::string file_name = root["file_name"].asString();
	const long long file_size = root["file_size"].asInt64();
	const std::string thumb = root["thumb"].asString();
	const std::string unique_id = root["unique_id"].asString();

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["fromuid"] = uid;
	response["touid"] = touid;
	response["msg_type"] = msg_type;
	response["name"] = name;
	response["unique_id"] = unique_id;

	//content 保存接收方渲染卡片所需的元数据；资源名与原始文件名分开，
	//服务器只按资源名落盘，展示名不参与路径拼接
	Json::Value meta;
	meta["name"] = file_name;
	meta["size"] = Json::Int64(file_size);
	meta["resource"] = name;
	meta["thumb"] = thumb;
	Json::FastWriter writer;
	std::string content = writer.write(meta);
	while (!content.empty()
		&& (content.back() == '\n' || content.back() == '\r' || content.back() == ' ')) {
		content.pop_back();
	}

	ChatMessageInfo saved;
	const int thread_type = thread_id > 0 ? ThreadTypeOf(thread_id)
		: static_cast<int>(ThreadType::Private);
	const std::string mentions = JoinMentions(root["mentions"]);
	const bool group = thread_type == static_cast<int>(ThreadType::Group);
	const bool saved_ok = group
		? MysqlDao::GetInstance()->SaveGroupResourceMessage(uid, thread_id, mentions, unique_id,
			msg_type, content, saved)
		: MysqlDao::GetInstance()->SaveResourceMessage(uid, touid, thread_id, unique_id,
			msg_type, content, saved);
	if (!saved_ok) {
		response["error"] = ErrorCodes::RPCFailed;
		response["group_invalid"] = group;
		session->Send(response.toStyledString(), ID_RESOURCE_CHAT_MSG_RSP);
		return;
	}

	response["thread_id"] = Json::Int64(saved.thread_id);
	response["thread_type"] = thread_type;
	response["message_id"] = Json::Int64(saved.message_id);
	response["status"] = 0; //资源尚未上传，等 ResourceServer 完成通知
	response["time"] = Json::Int64(saved.created_at);
	session->Send(response.toStyledString(), ID_RESOURCE_CHAT_MSG_RSP);
}

void LogicSystem::CreatePrivateChat(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);
	const int uid = root["fromuid"].asInt();
	const int peer_uid = root["touid"].asInt();
	Json::Value response;
	response["error"] = ErrorCodes::Success;
	response["uid"] = peer_uid;
	//打开会话时补拉一次: 登录后新加的好友可能还没有 thread_id
	const long long thread_id = MysqlDao::GetInstance()->GetOrCreatePrivateChat(uid, peer_uid);
	if (thread_id <= 0) {
		response["error"] = ErrorCodes::RPCFailed;
	} else {
		response["thread_id"] = Json::Int64(thread_id);
		auto peer = MysqlDao::GetInstance()->GetUser(peer_uid);
		if (peer) {
			response["name"] = peer->name;
			response["nick"] = peer->nick;
			response["icon"] = peer->icon;
			response["sex"] = peer->sex;
		}
	}
	session->Send(response.toStyledString(), ID_CREATE_PRIVATE_CHAT_RSP);
}

void LogicSystem::LoadChatThreads(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);
	const int uid = root["fromuid"].asInt();
	//客户端把上一页最后一条的 thread_id 与 time 一起发回来当复合游标；首次都为0。
	//只带 thread_id 的旧客户端仍走原来的升序游标，服务端保持兼容。
	const long long last_id = root["thread_id"].asInt64();
	const long long last_time = root.get("last_time", Json::Int64(0)).asInt64();
	std::vector<ChatThreadInfo> threads;
	bool has_more = false;
	Json::Value response;
	response["error"] = MysqlDao::GetInstance()->GetChatThreads(
		uid, last_id, 100, threads, has_more, last_time)
		? ErrorCodes::Success : ErrorCodes::RPCFailed;
	response["thread_list"] = Json::arrayValue;
	response["load_more"] = has_more;
	for (const auto& thread : threads)
		response["thread_list"].append(BuildThreadItem(thread));
	session->Send(response.toStyledString(), ID_LOAD_CHAT_THREAD_RSP);
}

void LogicSystem::LoadChatMessages(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);
	const int uid = root["fromuid"].asInt();
	const long long thread_id = root["thread_id"].asInt64();
	//只取比游标大的消息, 历史页和实时消息因此可以按 message_id 拼成一条有序序列
	const long long last_message_id = root["message_id"].asInt64();
	std::vector<ChatMessageInfo> messages;
	bool has_more = false;
	const bool success = MysqlDao::GetInstance()->GetChatMessages(
		uid, thread_id, last_message_id, 100, messages, has_more);
	const int thread_type = ThreadTypeOf(thread_id);
	Json::Value response;
	response["error"] = success ? ErrorCodes::Success : ErrorCodes::RPCFailed;
	response["thread_id"] = Json::Int64(thread_id);
	response["thread_type"] = thread_type;
	response["load_more"] = has_more;
	response["text_array"] = Json::arrayValue;
	for (const auto& message : messages) {
		Json::Value item;
		item["message_id"] = Json::Int64(message.message_id);
		item["thread_id"] = Json::Int64(message.thread_id);
		item["fromuid"] = message.sender_id;
		item["touid"] = message.recv_id;
		item["msgid"] = message.client_msg_id;
		item["content"] = message.content;
		item["msg_type"] = message.msg_type; //0 文本, 1 图片, 2 视频, 3 文件, 4 群系统消息
		item["time"] = Json::Int64(message.created_at); //UNIX 秒, 客户端按本地时区分层显示
		//自己发的消息带 read 表示对方是否已读；recalled 为真时只渲染一行提示，不显示正文
		item["read"] = message.is_read;
		item["recalled"] = message.recalled;
		item["mentions"] = message.mentions;
		//历史消息按"自己发的/别人发的"给默认状态；群聊不做逐条已读，自己发的都算已发送
		item["status"] = message.sender_id == uid ? 1 : 3;
		response["text_array"].append(item);
	}
	session->Send(response.toStyledString(), ID_LOAD_CHAT_MSG_RSP);
}

void LogicSystem::HeartBeatHandler(std::shared_ptr<CSession> session,
    const short&, const std::string&)
{
    //心跳的身份与字段已由 ValidateChatRequest 校验；这里不再解析 JSON，
    //3000 在线、10 秒一次心跳时，每秒 300 次无意义的解析与日志会被省掉
    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    session->Send(rtvalue.toStyledString(), ID_HEARTBEAT_RSP);
}

//主动登出：先让状态服务按 sid + epoch 吊销租约与令牌，再摘除本地登记并关闭连接。
//状态服务失败时本地照常下线，令牌由 7 天 TTL 兜底，不能因为控制面故障把用户卡在登录页
void LogicSystem::LogoutHandler(std::shared_ptr<CSession> session,
	const short&, const std::string&)
{
	Json::Value rsp;
	rsp["error"] = ErrorCodes::Success;
	rsp["revoked"] = false;

	const int uid = session->GetUserId();
	if (uid > 0) {
		auto reply = StatusGrpcClient::GetInstance()->LogoutSession(
			uid, session->GetSessionId(), session->GetEpoch());
		if (reply.error() != ErrorCodes::Success) {
			LOG_WARN << "[logout] revoke failed uid=" << uid
				<< " err=" << reply.error();
		}
		rsp["revoked"] = reply.revoked();
	}

	//先回包再摘除本地登记：ClearSession 之后这条连接不再接收业务消息
	session->Send(rsp.toStyledString(), ID_LOGOUT_RSP);
	_p_server->ClearSession(session->GetSessionId());
	session->CloseSoon(200);
}

//修改本人资料: 昵称、性别、个性签名。三个字段都可以为空, 空昵称表示界面回落到用户名
void LogicSystem::UpdateProfile(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_UPDATE_PROFILE_RSP);
		});

	//操作者只认会话绑定的uid, 请求体里不带uid, 客户端伪造就改不到别人的资料
	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	const std::string nick = root["nick"].asString();
	const std::string desc = root["desc"].asString();
	const int sex = root["sex"].asInt();
	//长度按UTF-8字符数再校验一次, 避免绕过界面直接发超长内容把列写坏
	if (Utf8CharCount(nick) > MAX_NICK_LEN || Utf8CharCount(desc) > MAX_DESC_LEN) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	if (!MysqlDao::GetInstance()->UpdateUserProfile(uid, nick, sex, desc)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}

	//资料缓存按uid和用户名各存一份, 写完立即覆盖; 在线好友搜索资料时直接读到新值
	auto user_info = MysqlDao::GetInstance()->GetUser(uid);
	if (user_info) {
		Json::Value base_root;
		base_root["uid"] = user_info->uid;
		base_root["name"] = user_info->name;
		base_root["email"] = user_info->email;
		base_root["nick"] = user_info->nick;
		base_root["desc"] = user_info->desc;
		base_root["sex"] = user_info->sex;
		base_root["icon"] = user_info->icon;
		RedisMgr::GetInstance()->Set(USER_BASE_INFO + std::to_string(uid),
			base_root.toStyledString());
		RedisMgr::GetInstance()->Set(NAME_INFO + user_info->name, base_root.toStyledString());
	}

	//回包带完整结果, 客户端据此刷新本地资料页与侧栏, 不必再发一次搜索
	rtvalue["uid"] = uid;
	rtvalue["nick"] = nick;
	rtvalue["sex"] = sex;
	rtvalue["desc"] = desc;

	//自己这一侧靠回包更新, 这里通知在线好友: 昵称与签名变了, 他们的列表与聊天标题跟着刷新
	NotifyFriendsProfileChanged(uid);
}

//资料变更推送给在线好友: 先查好友列表, 再按对方所在实例分发。
//不在线的好友不占一次 RPC, 他们下次登录时从 1006 回包里拿到最新资料。
void LogicSystem::NotifyFriendsProfileChanged(int uid)
{
	if (uid <= 0)
		return;

	//头像可能是资源服务器写的, 这里统一从库里读一次, 保证推送内容与落库一致
	auto user_info = MysqlDao::GetInstance()->GetUser(uid);
	if (!user_info)
		return;

	std::vector<int> friend_ids;
	if (!MysqlDao::GetInstance()->GetFriendIds(uid, friend_ids) || friend_ids.empty())
		return;

	Json::Value notify;
	notify["error"] = ErrorCodes::Success;
	notify["uid"] = uid;
	notify["name"] = user_info->name;
	notify["nick"] = user_info->nick;
	notify["desc"] = user_info->desc;
	notify["sex"] = user_info->sex;
	notify["icon"] = user_info->icon;
	const std::string notify_text = notify.toStyledString();

	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	//一次批量查询好友位置，避免好友数 × 一次 Redis/RPC
	const auto routes = SessionRouter::Inst().LookupBatch(friend_ids);
	std::map<std::string, std::vector<int>> remote;
	for (int friend_id : friend_ids) {
		if (friend_id <= 0)
			continue;

		auto route = routes.find(friend_id);
		if (route == routes.end())
			continue; //离线好友不推送，下次登录会从 1006/1050 拿到最新资料
		const std::string& ip = route->second;

		if (ip == self_name) {
			auto session = UserMgr::GetInstance()->GetSession(friend_id);
			if (session)
				session->Send(notify_text, ID_NOTIFY_PROFILE_CHANGED);
			continue;
		}
		remote[ip].push_back(friend_id);
	}

	for (const auto& entry : remote) {
		ProfileChangedReq auth_req;
		auth_req.set_fromuid(uid);
		for (int friend_id : entry.second)
			auth_req.add_touids(friend_id);
		auth_req.set_nick(user_info->nick);
		auth_req.set_desc(user_info->desc);
		auth_req.set_icon(user_info->icon);
		auth_req.set_sex(user_info->sex);
		ChatGrpcClient::GetInstance()->NotifyProfileChanged(entry.first, auth_req);
	}
}

//修改自己给某个好友设的备注: 只写自己这一侧, 对方那边的备注不受影响
void LogicSystem::UpdateFriendRemark(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_UPDATE_FRIEND_REMARK_RSP);
		});

	const int self_uid = session->GetUserId();
	const int friend_uid = root["touid"].asInt();
	const std::string remark = root["remark"].asString();
	//标签整组替换: 客户端提交的就是编辑后的完整集合, 空数组表示清空
	std::vector<std::string> tags;
	for (const auto& tag : root["tags"])
		tags.push_back(tag.asString());
	if (self_uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	//备注与昵称共用同一个长度上限, 清空备注同样允许
	if (Utf8CharCount(remark) > MAX_NICK_LEN) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	//返回值区分“不是好友”和“数据库错误”, 前者不能落成成功回包
	const int result = MysqlDao::GetInstance()->UpdateFriendRemark(self_uid, friend_uid, remark,
		tags);
	if (result == 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (result < 0) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}

	rtvalue["touid"] = friend_uid;
	rtvalue["remark"] = remark;
	//回包带回标签, 客户端用它刷新列表与筛选条
	rtvalue["tags"] = Json::arrayValue;
	for (const auto& tag : tags)
		rtvalue["tags"].append(tag);
}

//按 uid 取好友最新资料: 昵称和个性签名会被本人随时修改, 客户端打开会话或资料页时刷新一次
void LogicSystem::GetProfile(std::shared_ptr<CSession> session,
	const short& msg_id, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);
	const int touid = root["touid"].asInt();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_GET_PROFILE_RSP);
		});

	//只查公开资料, 不返回邮箱与密码; 目标不是好友也允许, 搜索结果同样需要
	std::string base_key = USER_BASE_INFO + std::to_string(touid);
	auto user_info = std::make_shared<UserInfo>();
	if (!usercache::GetBaseInfo(base_key, touid, user_info)) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	rtvalue["uid"] = user_info->uid;
	rtvalue["name"] = user_info->name;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;
}

//把会话事件推给对方：本实例直投，跨实例走 gRPC。
//对方离线时什么都不做——状态已经在库里，下次拉历史自然能看到已读/撤回
void LogicSystem::PushChatEvent(int from_uid, int to_uid, long long thread_id,
	long long message_id, int event)
{
	if (from_uid <= 0 || to_uid <= 0 || thread_id <= 0)
		return;
	//投递交给独立线程：对端实例不可达时 RPC 会等到截止时间，
	//已读/撤回通知晚几百毫秒不影响正确性
	PostDeliveryTask([this, from_uid, to_uid, thread_id, message_id, event]() {
		PushChatEventNow(from_uid, to_uid, thread_id, message_id, event);
	});
}

void LogicSystem::PushChatEventNow(int from_uid, int to_uid, long long thread_id,
	long long message_id, int event)
{
	if (from_uid <= 0 || to_uid <= 0 || thread_id <= 0)
		return;

	std::string ip;
	if (!SessionRouter::Inst().Lookup(to_uid, ip))
		return;

	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	if (ip == self_name) {
		auto peer = UserMgr::GetInstance()->GetSession(to_uid);
		if (!peer)
			return;

		Json::Value notify;
		notify["error"] = ErrorCodes::Success;
		notify["thread_id"] = Json::Int64(thread_id);
		notify["message_id"] = Json::Int64(message_id);
		if (event == 1) {
			//已读：客户端按 uid 确认是哪位好友读的
			notify["uid"] = from_uid;
			peer->Send(notify.toStyledString(), ID_NOTIFY_MSG_READ);
		}
		else {
			notify["fromuid"] = from_uid;
			peer->Send(notify.toStyledString(), ID_NOTIFY_MSG_RECALLED);
		}
		return;
	}

	ChatEventReq req;
	req.set_fromuid(from_uid);
	req.set_touid(to_uid);
	req.set_thread_id(thread_id);
	req.set_message_id(message_id);
	req.set_event(event);
	ChatGrpcClient::GetInstance()->NotifyChatEvent(ip, req);
}

//上报已读位置：客户端打开会话或收到新消息时调用，重复上报不会产生多余通知
void LogicSystem::MarkMessagesRead(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);
	const long long thread_id = root["thread_id"].asInt64();
	const long long message_id = root["message_id"].asInt64();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_MARK_READ_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (thread_id <= 0 || message_id <= 0) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	const int thread_type = ThreadTypeOf(thread_id);
	if (thread_type == static_cast<int>(ThreadType::Group)) {
		//群聊只推进自己的已读游标，未读数与会话行"有人@我"由它推导；
		//群里不展示逐条已读，所以不向其他成员推送
		if (!MysqlDao::GetInstance()->MarkGroupRead(uid, thread_id, message_id)) {
			rtvalue["error"] = ErrorCodes::RPCFailed;
			return;
		}
		rtvalue["thread_id"] = Json::Int64(thread_id);
		rtvalue["message_id"] = Json::Int64(message_id);
		return;
	}

	bool changed = false;
	int peer_uid = 0;
	if (!MysqlDao::GetInstance()->MarkChatMessagesRead(uid, thread_id, message_id, changed, peer_uid)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
	rtvalue["thread_id"] = Json::Int64(thread_id);
	rtvalue["message_id"] = Json::Int64(message_id);

	//没有新消息被标记时不必通知对方，避免每次切会话都推一遍
	if (changed)
		PushChatEvent(uid, peer_uid, thread_id, message_id, 1);
}

//撤回消息：自己这一侧靠回包更新，对方那一侧靠通知
void LogicSystem::RecallMessage(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);
	const long long message_id = root["message_id"].asInt64();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_RECALL_MSG_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (message_id <= 0) {
		rtvalue["error"] = ErrorCodes::Error_Json;
		return;
	}

	ChatMessageInfo message;
	const int code = MysqlDao::GetInstance()->RecallChatMessage(uid, message_id, message);
	if (code != ErrorCodes::Success) {
		rtvalue["error"] = code;
		return;
	}

	//thread_id 以库里为准，客户端传错也不会把通知发到别的会话
	rtvalue["thread_id"] = Json::Int64(message.thread_id);
	rtvalue["message_id"] = Json::Int64(message.message_id);
	rtvalue["thread_type"] = ThreadTypeOf(message.thread_id);
	if (rtvalue["thread_type"].asInt() == static_cast<int>(ThreadType::Group)) {
		//群内撤回：除自己外全部成员都换成提示行，离线成员登录拉历史时看到 recalled
		Json::Value payload;
		payload["error"] = ErrorCodes::Success;
		payload["thread_id"] = Json::Int64(message.thread_id);
		payload["thread_type"] = static_cast<int>(ThreadType::Group);
		payload["message_id"] = Json::Int64(message.message_id);
		payload["fromuid"] = uid;
		std::vector<int> members;
		if (MysqlDao::GetInstance()->GetGroupMemberIds(message.thread_id, members))
			BroadcastGroupEvent(message.thread_id, members, uid, payload, ID_NOTIFY_MSG_RECALLED);
		return;
	}
	PushChatEvent(uid, message.recv_id, message.thread_id, message.message_id, 2);
}

//删除好友：只改关系表，不推送、不通知。对方那边的好友列表要到下次登录才发现，
//期间还能看到历史会话；发消息时由 EnsureChatThread 的好友校验挡住
void LogicSystem::DeleteFriend(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Reader reader;
	Json::Value root;
	reader.parse(msg_data, root);
	const int touid = root["touid"].asInt();

	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["uid"] = touid; //失败时也带回来，客户端才能把乐观移除的那一行恢复
	Defer defer([&rtvalue, session]() {
		session->Send(rtvalue.toStyledString(), ID_DELETE_FRIEND_RSP);
		});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	if (touid <= 0 || touid == uid) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//只删自己这一行：对方仍把你看作好友，直到他也删除
	if (!MysqlDao::GetInstance()->DeleteFriend(uid, touid)) {
		rtvalue["error"] = ErrorCodes::RPCFailed;
		return;
	}
}

//群事件广播：本实例成员直接投递，其他实例按路由查询结果聚合后一次转发。
//离线的成员不补偿，消息与成员变更都已经落库，下次登录同步时能看到
void LogicSystem::BroadcastGroupEvent(long long thread_id, const std::vector<int>& members,
	int except_uid, const Json::Value& payload, int notify_id)
{
	if (members.empty())
		return;
	//本地直投与跨实例转发整体交给投递线程：群成员里只要有一个实例不可达，
	//同步 RPC 就会把业务线程按住数秒。payload/members 按值捕获，调用方返回后仍然有效
	PostDeliveryTask([this, thread_id, members, except_uid, payload, notify_id]() {
		BroadcastGroupEventNow(thread_id, members, except_uid, payload, notify_id);
	});
}

void LogicSystem::BroadcastGroupEventNow(long long thread_id, const std::vector<int>& members,
	int except_uid, const Json::Value& payload, int notify_id)
{
	if (members.empty())
		return;
	const std::string text = payload.toStyledString();
	const std::string self_name = ConfigMgr::Inst()["SelfServer"]["Name"];
	//一次批量查询所有成员的在线位置：群上限 200，避免 200 次单点查询
	const auto routes = SessionRouter::Inst().LookupBatch(members);
	std::map<std::string, std::vector<int>> remote;

	for (int uid : members) {
		if (uid <= 0 || uid == except_uid)
			continue;
		auto route = routes.find(uid);
		if (route == routes.end())
			continue;
		const std::string& instance = route->second;
		if (instance == self_name) {
			auto peer = UserMgr::GetInstance()->GetSession(uid);
			if (peer)
				peer->Send(text, static_cast<short>(notify_id));
		}
		else {
			remote[instance].push_back(uid);
		}
	}

	for (const auto& [instance, uids] : remote) {
		GroupEventReq req;
		req.set_thread_id(thread_id);
		req.set_notify_id(notify_id);
		req.set_payload(text);
		for (int uid : uids)
			req.add_touids(uid);
		ChatGrpcClient::GetInstance()->NotifyGroupEvent(instance, req);
	}
}

//群资料或成员变化：event 决定客户端怎么处理
//info 改名称/公告，members 人数与角色变化，created 新群，removed 我被移出，dismissed 群解散
void LogicSystem::PushGroupChanged(long long thread_id, const std::vector<int>& members,
	int except_uid, const std::string& event, const Json::Value& detail)
{
	Json::Value payload;
	payload["error"] = ErrorCodes::Success;
	payload["thread_id"] = Json::Int64(thread_id);
	payload["thread_type"] = static_cast<int>(ThreadType::Group);
	payload["event"] = event;
	//detail 里放本次变化涉及的字段，客户端按 event 取用，不再逐个事件定义协议
	for (const auto& key : detail.getMemberNames())
		payload[key] = detail[key];
	BroadcastGroupEvent(thread_id, members, except_uid, payload, ID_NOTIFY_GROUP_CHANGED);
}

//会话行序列化：单聊给 peer_*，群聊给 group_*；两类都带 thread_type、时间与未读
Json::Value LogicSystem::BuildThreadItem(const ChatThreadInfo& thread) const
{
	Json::Value item;
	item["thread_id"] = Json::Int64(thread.thread_id);
	item["thread_type"] = thread.thread_type;
	item["time"] = Json::Int64(thread.updated_at);
	item["unread"] = thread.unread;
	if (thread.thread_type == static_cast<int>(ThreadType::Group)) {
		item["group_name"] = thread.group_name;
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

//建群：创建者成为群主，勾选的好友一次性写入成员表；成功后给其他成员推 1068 created，
//他们收到后把群加进会话列表并拉一次历史，首屏就能看到"创建了群聊"这条系统消息
void LogicSystem::CreateGroup(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	bool replied = false; //成功路径会先回包再广播，错误路径仍由 Defer 兜底
	Defer defer([&response, session, &replied]() {
		if (!replied)
			session->Send(response.toStyledString(), ID_CREATE_GROUP_RSP);
	});

	const int uid = session->GetUserId();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}

	std::string name = root["name"].asString();
	//去掉首尾空白：只有空格的群名没有意义，客户端也会按空串回落到默认名
	const auto begin = name.find_first_not_of(" \t\r\n");
	const auto end = name.find_last_not_of(" \t\r\n");
	name = begin == std::string::npos ? "" : name.substr(begin, end - begin + 1);
	if (name.empty())
		name = "群聊";
	if (Utf8CharCount(name) > MAX_GROUP_NAME_LEN) {
		response["error"] = ErrorCodes::Error_Json;
		response["message"] = "群名称过长";
		return;
	}

	std::vector<int> members;
	if (root["members"].isArray()) {
		for (const auto& item : root["members"]) {
			const int member = item.asInt();
			if (member > 0)
				members.push_back(member);
		}
	}
	if (static_cast<int>(members.size()) > MAX_CREATE_GROUP_MEMBER) {
		response["error"] = ErrorCodes::GroupFull;
		response["message"] = "一次最多选择 50 位好友";
		return;
	}

	GroupInfo info;
	std::vector<GroupMemberInfo> member_list;
	int error = ErrorCodes::Success;
	const long long thread_id = MysqlDao::GetInstance()->CreateGroup(uid, name, members,
		info, member_list, error);
	if (thread_id <= 0) {
		response["error"] = error;
		response["message"] = error == ErrorCodes::NotFriend ? "只能邀请自己的好友"
			: (error == ErrorCodes::GroupFull ? "群成员已达上限" : "建群失败");
		return;
	}

	spdlog::info("group created thread={} owner={} members={}", thread_id, uid, info.member_count);
	response["thread_id"] = Json::Int64(thread_id);
	response["thread_type"] = static_cast<int>(ThreadType::Group);
	Json::Value group_json;
	group_json["thread_id"] = Json::Int64(info.thread_id);
	group_json["name"] = info.name;
	group_json["notice"] = info.notice;
	group_json["owner"] = info.owner_id;
	group_json["member_count"] = info.member_count;
	group_json["my_role"] = info.my_role;
	group_json["muted"] = false;
	group_json["my_nick"] = "";
	group_json["created_at"] = Json::Int64(info.created_at);
	response["group"] = group_json;
	response["members"] = Json::arrayValue;
	for (const auto& member : member_list) {
		Json::Value item;
		item["uid"] = member.uid;
		item["name"] = member.name;
		item["nick"] = member.nick;
		item["icon"] = member.icon;
		item["sex"] = member.sex;
		item["role"] = member.role;
		item["group_nick"] = member.group_nick;
		response["members"].append(item);
	}

	//先回包再广播：创建者如果先收到 1068/1019，客户端还没有这个会话，系统消息会被丢掉
	session->Send(response.toStyledString(), ID_CREATE_GROUP_RSP);
	replied = true;

	//其他成员：把群资料与成员一起推过去，客户端据此立刻出现会话
	std::vector<int> ids;
	for (const auto& member : member_list)
		ids.push_back(member.uid);
	Json::Value detail;
	detail["group"] = group_json;
	detail["members"] = response["members"];
	PushGroupChanged(thread_id, ids, uid, "created", detail);

	//首条系统消息由服务端写，文案里带上创建者，成员翻历史时能看懂群的来历。
	//这条要发给包括群主在内的所有人：发送者本地没有这条消息
	const std::string text = [&]() {
		auto self = MysqlDao::GetInstance()->GetUser(uid);
		const std::string actor = self ? (self->nick.empty() ? self->name : self->nick)
			: std::to_string(uid);
		return actor + " 创建了群聊，快和大家打个招呼吧";
	}();
	ChatMessageInfo message;
	if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
		Json::Value payload;
		payload["error"] = ErrorCodes::Success;
		payload["thread_id"] = Json::Int64(thread_id);
		payload["thread_type"] = static_cast<int>(ThreadType::Group);
		payload["event"] = "message";
		payload["message"] = BuildMessageItem(message, 1);
		BroadcastGroupEvent(thread_id, ids, 0, payload, ID_NOTIFY_GROUP_CHANGED);
	}
}

void LogicSystem::GetGroupInfo(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(response.toStyledString(), ID_GET_GROUP_INFO_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);

	GroupInfo info;
	std::vector<GroupMemberInfo> members;
	if (thread_id <= 0 || !MysqlDao::GetInstance()->GetGroupInfo(uid, thread_id, info, members)) {
		response["error"] = ErrorCodes::GroupNotFound;
		return;
	}

	Json::Value group_json;
	group_json["thread_id"] = Json::Int64(info.thread_id);
	group_json["name"] = info.name;
	group_json["notice"] = info.notice;
	group_json["owner"] = info.owner_id;
	group_json["member_count"] = info.member_count;
	group_json["my_role"] = info.my_role;
	group_json["muted"] = info.muted;
	group_json["my_nick"] = info.my_nick;
	group_json["created_at"] = Json::Int64(info.created_at);
	//昵称、头像、性别都来自 user 表，单独带在 members 里，客户端不需要再逐个拉资料
	response["group"] = group_json;
	response["members"] = Json::arrayValue;
	for (const auto& member : members) {
		Json::Value item;
		item["uid"] = member.uid;
		item["name"] = member.name;
		item["nick"] = member.nick;
		item["icon"] = member.icon;
		item["sex"] = member.sex;
		item["role"] = member.role;
		item["group_nick"] = member.group_nick;
		response["members"].append(item);
	}
}

//群设置：name/notice 改群资料，nickname 改我在本群的昵称，mute 切换免打扰。
//群资料变更要推给其他成员，个人设置只回自己
void LogicSystem::UpdateGroup(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(response.toStyledString(), ID_UPDATE_GROUP_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string action = root["action"].asString();
	const std::string value = root["value"].asString();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);
	response["action"] = action;

	MysqlDao::GroupField field;
	if (action == "name") {
		if (value.empty() || Utf8CharCount(value) > MAX_GROUP_NAME_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群名称需为 1-30 个字符";
			return;
		}
		field = MysqlDao::GroupField::Name;
	}
	else if (action == "notice") {
		if (Utf8CharCount(value) > MAX_GROUP_NOTICE_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群公告最多 200 个字符";
			return;
		}
		field = MysqlDao::GroupField::Notice;
	}
	else if (action == "nickname") {
		if (Utf8CharCount(value) > MAX_GROUP_NICK_LEN) {
			response["error"] = ErrorCodes::Error_Json;
			response["message"] = "群昵称最多 16 个字符";
			return;
		}
		field = MysqlDao::GroupField::MyNickname;
	}
	else if (action == "mute") {
		field = MysqlDao::GroupField::Muted;
	}
	else {
		response["error"] = ErrorCodes::Error_Json;
		return;
	}

	std::string applied;
	const int code = MysqlDao::GetInstance()->UpdateGroupField(thread_id, uid, field, value, applied);
	if (code != ErrorCodes::Success) {
		response["error"] = code;
		response["message"] = code == ErrorCodes::GroupDenied ? "只有群主或管理员可以修改"
			: "群不存在或你已不在群里";
		return;
	}
	response["value"] = applied;

	//群名与公告要让所有成员看到；群昵称与免打扰是个人设置，不推送
	if (action == "name" || action == "notice") {
		std::vector<int> members;
		if (!MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members))
			return;
		Json::Value detail;
		detail[action] = applied;
		PushGroupChanged(thread_id, members, uid, "info", detail);

		//系统消息留痕：改群名与改公告都写一行，成员翻历史时能看懂发生了什么
		auto self = MysqlDao::GetInstance()->GetUser(uid);
		const std::string actor = self ? (self->nick.empty() ? self->name : self->nick)
			: std::to_string(uid);
		const std::string text = action == "name"
			? actor + " 修改群名为「" + applied + "」"
			: actor + " 更新了群公告";
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
	}
}

//成员操作：邀请、移除、设/撤管理员、退群、解散。
//每个动作成功后都会写一条系统消息并广播"成员数变化"，让所有在线成员的资料页同步
void LogicSystem::GroupMemberOp(std::shared_ptr<CSession> session,
	const short&, const std::string& msg_data)
{
	Json::Value root;
	Json::Reader reader;
	reader.parse(msg_data, root);

	Json::Value response;
	response["error"] = ErrorCodes::Success;
	Defer defer([&response, session]() {
		session->Send(response.toStyledString(), ID_GROUP_MEMBER_OP_RSP);
	});

	const int uid = session->GetUserId();
	const long long thread_id = root["thread_id"].asInt64();
	const std::string action = root["action"].asString();
	if (uid <= 0) {
		response["error"] = ErrorCodes::UidInvalid;
		return;
	}
	response["thread_id"] = Json::Int64(thread_id);
	response["action"] = action;

	auto userName = [](int member_uid) {
		auto user = MysqlDao::GetInstance()->GetUser(member_uid);
		if (!user)
			return std::string("成员");
		return user->nick.empty() ? user->name : user->nick;
	};

	if (action == "invite") {
		std::vector<int> invited;
		if (root["members"].isArray()) {
			for (const auto& item : root["members"]) {
				if (item.asInt() > 0)
					invited.push_back(item.asInt());
			}
		}
		if (invited.empty()) {
			response["error"] = ErrorCodes::Error_Json;
			return;
		}
		std::vector<int> added;
		int member_count = 0;
		const int code = MysqlDao::GetInstance()->InviteGroupMembers(thread_id, uid, invited,
			added, member_count);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupFull ? "群成员已达上限"
				: (code == ErrorCodes::NotFriend ? "只能邀请自己的好友" : "邀请失败");
			return;
		}
		response["member_count"] = member_count;
		response["added"] = Json::arrayValue;
		std::string names;
		for (int member : added) {
			response["added"].append(member);
			if (!names.empty())
				names += "、";
			names += userName(member);
		}
		if (added.empty()) {
			response["message"] = "对方已经在群里";
			return;
		}

		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		const std::string text = userName(uid) + " 邀请 " + names + " 加入了群聊";
		ChatMessageInfo message;
		Json::Value detail;
		detail["member_count"] = member_count;
		PushGroupChanged(thread_id, members, uid, "members", detail);
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "remove" || action == "set_admin" || action == "unset_admin") {
		const int target = root["target"].asInt();
		if (target <= 0 || target == uid) {
			response["error"] = ErrorCodes::UidInvalid;
			return;
		}
		response["target"] = target;
		int code = ErrorCodes::Success;
		int member_count = 0;
		std::string text;
		if (action == "remove") {
			code = MysqlDao::GetInstance()->RemoveGroupMember(thread_id, uid, target, member_count);
			text = userName(uid) + " 将 " + userName(target) + " 移出群聊";
		}
		else {
			const bool admin = action == "set_admin";
			code = MysqlDao::GetInstance()->SetGroupAdmin(thread_id, uid, target, admin);
			text = userName(uid) + (admin ? " 将 " : " 取消 ") + userName(target)
				+ (admin ? " 设为管理员" : " 的管理员");
		}
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "没有操作权限"
				: "成员不存在或群已解散";
			return;
		}
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		if (action == "remove") {
			response["member_count"] = member_count;
			Json::Value detail;
			detail["member_count"] = member_count;
			PushGroupChanged(thread_id, members, uid, "members", detail);
			//被移除的人单独收到 removed，客户端据此把会话从列表里拿掉
			Json::Value removed;
			removed["target"] = target;
			std::vector<int> one{target};
			PushGroupChanged(thread_id, one, 0, "removed", removed);
		}
		else {
			Json::Value detail;
			detail["target"] = target;
			detail["role"] = action == "set_admin" ? static_cast<int>(GroupRole::Admin)
				: static_cast<int>(GroupRole::Member);
			PushGroupChanged(thread_id, members, uid, "members", detail);
		}
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid, text, message)) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "leave") {
		bool dismissed = false;
		const int code = MysqlDao::GetInstance()->LeaveGroup(thread_id, uid, dismissed);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "群主请先解散群聊" : "退群失败";
			return;
		}
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		Json::Value detail;
		detail["target"] = uid;
		if (!members.empty())
			PushGroupChanged(thread_id, members, uid, "members", detail);
		ChatMessageInfo message;
		if (MysqlDao::GetInstance()->AddGroupSystemMessage(thread_id, uid,
			userName(uid) + " 退出了群聊", message) && !members.empty()) {
			Json::Value payload;
			payload["error"] = ErrorCodes::Success;
			payload["thread_id"] = Json::Int64(thread_id);
			payload["thread_type"] = static_cast<int>(ThreadType::Group);
			payload["event"] = "message";
			payload["message"] = BuildMessageItem(message, 1);
			BroadcastGroupEvent(thread_id, members, 0, payload, ID_NOTIFY_GROUP_CHANGED);
		}
		return;
	}

	if (action == "dismiss") {
		//解散是软删：成员行与消息都保留，这里取成员列表只为把变更推给他们
		std::vector<int> members;
		MysqlDao::GetInstance()->GetGroupMemberIds(thread_id, members);
		const int code = MysqlDao::GetInstance()->DismissGroup(thread_id, uid);
		if (code != ErrorCodes::Success) {
			response["error"] = code;
			response["message"] = code == ErrorCodes::GroupDenied ? "只有群主可以解散群聊" : "解散失败";
			return;
		}
		spdlog::info("group dismissed thread={} owner={} members={}",
			thread_id, uid, members.size());
		Json::Value detail;
		PushGroupChanged(thread_id, members, 0, "dismissed", detail);
		return;
	}

	response["error"] = ErrorCodes::Error_Json;
}

bool LogicSystem::isPureDigit(const std::string& str)
{
    if (str.empty()) return false;
	for (unsigned char c : str) {
		//如果字符串中的某个字符不是数字，则返回false，说明字符串不是纯数字
		if (!std::isdigit(c)) {
			return false;
		}
	}
	return true;
}

void LogicSystem::GetUserByUid(std::string uid_str, Json::Value& rtvalue)
{
	rtvalue["error"] = ErrorCodes::Success; //默认返回成功状态码
	std::string base_key = USER_BASE_INFO + uid_str; //构建用户基本信息在Redis中的键名

	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str); //从Redis中获取用户基本信息字符串
	if (b_base) {
		Json::Reader reader; //创建Json解析器对象
		Json::Value root; //创建Json值对象，用于存储解析后的数据
		reader.parse(info_str, root); //解析Json字符串，将结果存储在root对象中
		//从解析后的Json对象中获取用户基本信息
		auto uid = root["uid"].asInt();
		auto name = root["name"].asString();
		auto email = root["email"].asString();
		auto nick = root["nick"].asString();
		auto desc = root["desc"].asString();
		auto sex = root["sex"].asInt();
		auto icon = root["icon"].asString();

		//将用户基本信息填充到返回值Json对象中，准备返回给客户端
		rtvalue["uid"] = uid;
		rtvalue["name"] = name;
		rtvalue["email"] = email;
		rtvalue["nick"] = nick;
		rtvalue["desc"] = desc;
		rtvalue["sex"] = sex;
		rtvalue["icon"] = icon;
		return;
	}

	int uid = 0;
    //纯数字字符串也可能超出int范围，from_chars通过错误码报告失败，不会像stoi一样抛出异常
    auto [end, error] = std::from_chars(uid_str.data(), uid_str.data() + uid_str.size(), uid);
    if (error != std::errc{} || end != uid_str.data() + uid_str.size() || uid <= 0) {
        rtvalue["error"] = ErrorCodes::UidInvalid;
        return;
    }
	//redis中没有则查询mysql
	std::shared_ptr<UserInfo> user_info = nullptr;
	user_info = MysqlDao::GetInstance()->GetUser(uid); //根据用户ID从MySQL数据库中获取用户信息
	//如果数据库中没有用户信息，则返回uid无效错误码
	if (user_info == nullptr) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//将数据库内容写入redis缓存
	Json::Value redis_root;
	redis_root["uid"] = user_info->uid;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	redis_root["icon"] = user_info->icon;
	RedisMgr::GetInstance()->Set(base_key, redis_root.toStyledString()); //将用户基本信息写入Redis缓存

	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
	rtvalue["icon"] = user_info->icon;
}

void LogicSystem::GetUserByName(std::string name, Json::Value& rtvalue)
{
	rtvalue["error"] = ErrorCodes::Success; //默认返回成功状态码
	std::string base_key = NAME_INFO + name; //构建用户基本信息在Redis中的键名

	//优先查redis中查询用户信息
	std::string info_str = "";
	bool b_base = RedisMgr::GetInstance()->Get(base_key, info_str); //从Redis中获取用户基本信息字符串
	if (b_base) {
		Json::Reader reader; //创建Json解析器对象
		Json::Value root; //创建Json值对象，用于存储解析后的数据
		reader.parse(info_str, root); //解析Json字符串，将结果存储在root对象中
		//从解析后的Json对象中获取用户基本信息
		auto uid = root["uid"].asInt();
		auto name = root["name"].asString();
		auto email = root["email"].asString();
		auto nick = root["nick"].asString();
		auto desc = root["desc"].asString();
		auto sex = root["sex"].asInt();

		//将用户基本信息填充到返回值Json对象中，准备返回给客户端
		rtvalue["uid"] = uid;
		rtvalue["name"] = name;
		rtvalue["email"] = email;
		rtvalue["nick"] = nick;
		rtvalue["desc"] = desc;
		rtvalue["sex"] = sex;
		return;
	}

	//redis中没有则查询mysql
	std::shared_ptr<UserInfo> user_info = nullptr;
	user_info = MysqlDao::GetInstance()->GetUser(name); //根据用户名从MySQL数据库中获取用户信息
	//如果数据库中没有用户信息，则返回uid无效错误码
	if (user_info == nullptr) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}

	//将数据库内容写入redis缓存
	Json::Value redis_root;
	redis_root["uid"] = user_info->uid;
	redis_root["name"] = user_info->name;
	redis_root["email"] = user_info->email;
	redis_root["nick"] = user_info->nick;
	redis_root["desc"] = user_info->desc;
	redis_root["sex"] = user_info->sex;
	RedisMgr::GetInstance()->Set(base_key, redis_root.toStyledString()); //将用户基本信息写入Redis缓存

	//返回数据
	rtvalue["uid"] = user_info->uid;
	rtvalue["name"] = user_info->name;
	rtvalue["email"] = user_info->email;
	rtvalue["nick"] = user_info->nick;
	rtvalue["desc"] = user_info->desc;
	rtvalue["sex"] = user_info->sex;
}
bool LogicSystem::GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>>& list)
{
	//从mysql获取好友申请列表
	return MysqlDao::GetInstance()->GetApplyList(to_uid, list, 0, 10);
}

bool LogicSystem::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>>& user_list)
{
	//从mysql获取好友列表
	return MysqlDao::GetInstance()->GetFriendList(self_id, user_list);
}
