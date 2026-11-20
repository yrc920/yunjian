#include "LogicSystem.h"
#include "UserCache.h"
#include "StatusGrpcClient.h"
#include "MysqlDao.h"
#include "JsonUtil.h"
#include "RedisMgr.h"
#include "UserMgr.h"
#include "SessionRouter.h"
#include "ConfigMgr.h"
#include "CServer.h"
#include <ctime>
#include "Log.h"

void LogicSystem::LoginHandler(std::shared_ptr<CSession> session,
	const short&, const Json::Value& root)
{

	auto uid = root["uid"].asInt(); //从解析后的Json对象中获取用户ID
	auto token = root["token"].asString(); //从解析后的Json对象中获取用户token

	Json::Value rtvalue; //创建Json值对象，用于存储返回给客户端的数据
	Defer defer([&rtvalue, session]() {
		//将Json值对象转换为字符串形式，准备发送给客户端
		std::string return_str = JsonToCompact(rtvalue);
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

	UserInfo user_info;
	if (!usercache::Get(uid, user_info)) {
		rtvalue["error"] = ErrorCodes::UidInvalid;
		return;
	}
	rtvalue["uid"] = uid;
	rtvalue["name"] = user_info.name;
	rtvalue["email"] = user_info.email;
	rtvalue["nick"] = user_info.nick;
	rtvalue["desc"] = user_info.desc;
	rtvalue["sex"] = user_info.sex;
	rtvalue["icon"] = user_info.icon;

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
	GetFriendList(uid, friend_list);
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

void LogicSystem::HeartBeatHandler(std::shared_ptr<CSession> session,
    const short&, const Json::Value&)
{
    //心跳的身份与字段已由 ValidateChatRequest 校验；这里不再解析 JSON，
    //3000 在线、10 秒一次心跳时，每秒 300 次无意义的解析与日志会被省掉
    Json::Value rtvalue;
    rtvalue["error"] = ErrorCodes::Success;
    session->Send(JsonToCompact(rtvalue), ID_HEARTBEAT_RSP);
}

//主动登出：先让状态服务按 sid + epoch 吊销租约与令牌，再摘除本地登记并关闭连接。
//状态服务失败时本地照常下线，令牌由 7 天 TTL 兜底，不能因为控制面故障把用户卡在登录页
void LogicSystem::LogoutHandler(std::shared_ptr<CSession> session,
	const short&, const Json::Value&)
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
	session->Send(JsonToCompact(rsp), ID_LOGOUT_RSP);
	_p_server->ClearSession(session->GetSessionId());
	session->CloseSoon(200);
}
