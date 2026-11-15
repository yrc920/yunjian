#pragma once
// 逻辑系统类, 负责处理业务逻辑
//

#include "Singleton.h"
#include "CSession.h"
#include "data.h"
#include "const.h"
#include <queue>
#include <thread>
#include <json/json.h>
#include <json/value.h>
#include <json/reader.h>
#include <map>
#include <unordered_map>
#include <condition_variable>

//定义一个函数类型，用于处理不同消息ID的回调函数，参数包括会话对象、消息ID和消息数据
typedef std::function<void(std::shared_ptr<CSession>,
	const short& msg_id, const std::string& msg_data)> FunCallBack;

class LogicSystem :public Singleton<LogicSystem>
{
	friend class Singleton<LogicSystem>; //允许Singleton类访问LogicSystem的私有成员
public:
	~LogicSystem(); //使基类析构时可以访问派生类的析构函数
    void Stop(); //由主线程停服，在 Redis 与 io_context 销毁前结束业务线程
	void PostMsgToQue(std::shared_ptr<LogicNode> msg); //将消息节点添加到消息队列中，供逻辑处理线程处理
    void SetServer(std::shared_ptr<CServer> pserver);
	//资料变更后推给在线好友: 改资料成功后由逻辑层调用, 换头像时经 gRPC 从资源服务器触发
	void NotifyFriendsProfileChanged(int uid);
	//群事件广播：本实例成员直接投递，其他实例按 uip_ 聚合后走一次 gRPC。
	//except_uid 用于跳过发送者；notify_id 是客户端协议号(1019/1033/1036/1057/1068)
	void BroadcastGroupEvent(long long thread_id, const std::vector<int>& members,
		int except_uid, const Json::Value& payload, int notify_id);

private:
	LogicSystem();
	void DealMsg(); //逻辑处理线程执行的函数，负责从消息队列中取出消息并调用对应的处理函数进行处理
	void RegisterCallBacks(); //注册消息处理函数
	//登录处理函数, 处理用户登录请求，验证用户身份并返回登录结果
	void LoginHandler(std::shared_ptr<CSession>, const short& msg_id, const std::string& msg_data);
	//搜索用户信息处理函数，处理用户搜索请求并返回搜索结果
	void SearchInfo(std::shared_ptr<CSession>, const short& msg_id, const std::string& msg_data);
	//申请添加好友处理函数，处理用户申请添加好友的请求并返回处理结果
	void AddFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//同意好友申请: 建立双向好友关系, 建会话并写入两条欢迎消息
	void AuthFriendApply(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//文本消息: 先落库再回执, 对方在线时实时投递
	void DealChatTextMsg(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//图片消息: 先落库占位, 回执后客户端再把资源上传到 ResourceServer
	void DealChatImgMsg(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//视频/文件消息: 落库时写入文件名、大小与视频首帧资源, 资源随后上传
	void DealResourceChatMsg(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//打开单聊会话: 按需创建 thread 并返回对方资料
	void CreatePrivateChat(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//按 thread_id 游标分页拉会话列表
	void LoadChatThreads(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//按 message_id 游标分页拉会话历史
	void LoadChatMessages(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
    //心跳处理函数，处理用户心跳请求并返回心跳响应
    void HeartBeatHandler(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//修改本人资料: 昵称、性别、个性签名, 成功后同步刷新资料缓存
	void UpdateProfile(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//修改自己给好友设的备注与标签, 只写自己这一侧的关系行
	void UpdateFriendRemark(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//按 uid 取好友最新资料, 供聊天标题与资料页刷新昵称和个性签名
	void GetProfile(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//上报已读：把该会话中发给自己的消息标成已读，并通知对方把单勾改成双勾
	void MarkMessagesRead(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//撤回自己发的消息：2 分钟内有效，成功后通知对方把气泡换成"撤回了一条消息"
	void RecallMessage(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//会话事件分发：event 1 已读 / 2 撤回；本实例直投，跨实例走 gRPC
	void PushChatEvent(int from_uid, int to_uid, long long thread_id, long long message_id, int event);
	//删除好友：双向删关系，不通知对方，会话与历史保留
	void DeleteFriend(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//创建群聊：群名 + 勾选的好友，创建者成为群主
	void CreateGroup(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//拉群资料与成员，同时带回我的角色、群昵称与免打扰
	void GetGroupInfo(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//改群名/群公告、改我在本群的昵称、切换免打扰
	void UpdateGroup(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//群成员操作：邀请、移除、设/撤管理员、退群、解散
	void GroupMemberOp(std::shared_ptr<CSession> session, const short& msg_id, const std::string& msg_data);
	//群资料或成员变化后推送 1068：event 为 info/members/removed/dismissed
	void PushGroupChanged(long long thread_id, const std::vector<int>& members, int except_uid,
		const std::string& event, const Json::Value& detail);
	//按 thread_id 判断会话类型；查询失败时按单聊处理，保持原有链路可用
	int ThreadTypeOf(long long thread_id);
	//把一条已落库的消息组装成客户端通知里的字段，文本/图片/视频文件共用
	Json::Value BuildMessageItem(const ChatMessageInfo& message, int status);
	//会话行统一序列化：登录首屏与 1028 分页共用，避免两处字段不一致
	Json::Value BuildThreadItem(const ChatThreadInfo& thread) const;

	bool isPureDigit(const std::string& str); //判断字符串是否纯数字
	//根据用户ID查询用户信息，并将结果填充到Json值对象中
	void GetUserByUid(std::string uid_str, Json::Value& rtvalue);
	//根据用户名查询用户信息，并将结果填充到Json值对象中
	void GetUserByName(std::string name, Json::Value& rtvalue);
	//获取用户基本信息
	bool GetBaseInfo(std::string base_key, int uid, std::shared_ptr<UserInfo>& userinfo);
	//获取好友申请信息列表
	bool GetFriendApplyInfo(int to_uid, std::vector<std::shared_ptr<ApplyInfo>>& list);
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo>>& user_list);

	std::thread _worker_thread; //逻辑处理线程
	std::queue<std::shared_ptr<LogicNode>> _msg_que; //逻辑消息队列
	std::mutex _mutex; //保护消息队列的互斥锁
	std::condition_variable _consume; //条件变量，用于通知逻辑处理线程有新消息到来
	bool _b_stop; //标志逻辑系统是否停止，控制逻辑处理线程的退出

	//消息ID到处理函数的映射表，用于根据消息ID分发消息到对应的处理函数
	std::map<short, FunCallBack> _fun_callbacks;
    std::shared_ptr<CServer> _p_server; //CServer对象的智能指针，用于在逻辑处理过程中访问服务器相关功能
};
