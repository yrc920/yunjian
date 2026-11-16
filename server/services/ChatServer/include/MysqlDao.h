#pragma once
// 数据库访问对象类, 用于封装对MySQL数据库的操作
//

#include "const.h"
#include <thread>
#include <mysqlx/xdevapi.h>
#include "data.h"
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
class SqlConnection; //封装MySQL连接对象, 包含连接对象和上次操作时间(前置声明)
class MySqlPool; //MySQL连接池, 提供获取和归还连接的接口(前置声明)

class MysqlDao
{
public:
	MysqlDao(); //初始化MySQL连接池
	~MysqlDao();
	//注册用户, 返回用户ID, 如果注册失败则返回-1
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);
	bool UpdatePwd(const std::string& name, const std::string& newpwd); //更新密码
	bool CheckEmail(const std::string& name, const std::string& email); //检查用户名和邮箱是否匹配
	//检查用户名和密码是否匹配, 如果匹配则将用户信息存储在userInfo中
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo);
	bool AddFriendApply(const int& from, const int& to, const std::string& desc,
		const std::string& back_name, const std::vector<std::string>& tags); //添加好友申请、留言、备注与标签
	//建立双向好友关系并置申请为已通过; 输出申请方当初给通过方设的备注与标签。
	//first_approve 表示这次调用之前申请还是未处理状态, 用来避免重复同意时再写一遍欢迎语
	bool AddFriend(const int& from, const int& to, std::string back_name,
		const std::vector<std::string>& tags, std::string& apply_desc,
		std::string& reverse_back, std::vector<std::string>& reverse_tags,
		bool& first_approve);
	//更新本人可编辑的资料字段; 昵称与签名的长度由调用方按字符数校验
	bool UpdateUserProfile(int uid, const std::string& nick, int sex, const std::string& desc);
	//修改自己给好友设的备注与标签: 返回 1 已更新, 0 不是好友, -1 数据库错误
	int UpdateFriendRemark(int self_id, int friend_id, const std::string& remark,
		const std::vector<std::string>& tags);
	//好友表与申请表各有一个 tags 列: 启动时幂等补列, 不需要部署环境手工建表
	//启动时补齐后加的列（好友标签、消息已读/撤回），已存在则跳过
	bool EnsureSchema();

	std::shared_ptr<UserInfo> GetUser(int uid); //根据用户ID获取用户信息
	std::shared_ptr<UserInfo> GetUser(const std::string& name); //根据用户名获取用户信息
	//获取好友申请列表, 根据起始id和限制条数返回列表
	bool GetApplyList(int touid,
		std::vector<std::shared_ptr<ApplyInfo>>& applyList, int offset, int limit);
	//获取我发出的好友申请, 按申请id升序分页; 与收到方向分开展示
	bool GetSentApplyList(int from_uid,
		std::vector<std::shared_ptr<ApplyInfo>>& applyList, int offset, int limit);
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);
	//取某人的好友 uid 列表, 资料变更后按它决定推给谁
	bool GetFriendIds(int uid, std::vector<int>& friend_ids);
	//把该会话里"发给 uid"的消息标为已读；changed 表示是否真有更新，peer_uid 输出对方 uid
	bool MarkChatMessagesRead(int uid, long long thread_id, long long last_message_id,
		bool& changed, int& peer_uid);
	//撤回自己发的消息：返回 ErrorCodes，成功时把撤回后的消息写进 message
	int RecallChatMessage(int uid, long long message_id, ChatMessageInfo& message);
	//两个人当前的关系：0 无关系, 1 互为好友, 2 我已申请待通过, 3 对方申请待我处理, 4 单向(被删过)
	int GetFriendRelation(int me, int peer);
	//双方是否还是好友：删好友后用于判断消息能否发出
	bool IsFriend(int uid, int touid);
	//双向关系是否完整（两行都在）：发消息的前提
	bool IsMutualFriend(int uid, int touid);
	//删除好友：只删自己这一行，对方仍保留关系
	bool DeleteFriend(int uid, int touid);
	//取单聊会话, 没有则创建; 非好友返回0. 依赖 private_chat 的 (user1_id,user2_id) 唯一键去重
	long long GetOrCreatePrivateChat(int user1, int user2);
	//按会话ID升序分页取会话列表, has_more 表示还有下一页
	bool GetChatThreads(int uid, long long last_thread_id, int limit,
		std::vector<ChatThreadInfo>& threads, bool& has_more);
	//按消息ID升序分页取会话内消息, 只允许会话成员读取
	bool GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
		std::vector<ChatMessageInfo>& messages, bool& has_more);
	//批量写入消息; thread_id 为0时先找到或创建会话, 相同 (会话,发送者,client_msg_id) 视为重发
	bool SaveChatMessages(int sender_id, int recv_id, long long thread_id,
		const std::vector<std::pair<std::string, std::string>>& contents,
		std::vector<ChatMessageInfo>& messages);
	//写入一条资源消息(图片/视频/文件): 资源还没上传, 先落库占位, 由资源服务器完成后回写状态
	bool SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	//资源上传完成后按消息ID取类型与内容, 用于给接收方拼通知
	bool GetChatMessage(long long message_id, ChatMessageInfo& message);
	//资源服务器通知: 把指定消息的状态改成已上传; sender 用于确认消息归属
	bool UpdateChatMessageStatus(long long message_id, int sender, int status);
	//会话类型: 1 单聊 / 2 群聊; 不存在返回 false
	bool GetThreadType(long long thread_id, int& thread_type);

	//建群: 群主 + 勾选的好友(必须是群主的好友), 同时写 chat_thread/group_chat/成员与首条系统消息。
	//返回 thread_id, 0 表示失败; error 带回具体原因
	long long CreateGroup(int owner_id, const std::string& group_name,
		const std::vector<int>& members, GroupInfo& info,
		std::vector<GroupMemberInfo>& member_list, int& error);
	//群资料与成员: 只允许群成员读取, 同时带回我的角色、群昵称与免打扰设置
	bool GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
		std::vector<GroupMemberInfo>& members);
	//群设置字段: 与 group_chat / group_chat_member 的列一一对应
	enum class GroupField { Name = 1, Notice = 2, MyNickname = 3, Muted = 4 };
	//修改群设置: 群名与公告需要管理员及以上, 群昵称与免打扰改自己那一行。
	//返回 ErrorCodes; 成功时 value 回填生效后的值
	int UpdateGroupField(long long thread_id, int uid, GroupField field,
		const std::string& value, std::string& applied);
	//邀请成员: 只能邀请自己的好友, 已在群里的跳过; added 返回真正加入的人, count 返回新人数
	int InviteGroupMembers(long long thread_id, int operator_uid,
		const std::vector<int>& members, std::vector<int>& added, int& member_count);
	//移除成员: 群主可移除任何人(含管理员), 管理员只能移除普通成员, 不能移除群主
	int RemoveGroupMember(long long thread_id, int operator_uid, int target_uid,
		int& member_count);
	//设置或取消管理员: 只有群主可以操作, 目标必须是群成员
	int SetGroupAdmin(long long thread_id, int operator_uid, int target_uid, bool admin);
	//退群: 群主不能直接退群(需先解散); dismissed 恒为 false, 保留字段供上层扩展
	int LeaveGroup(long long thread_id, int uid, bool& dismissed);
	//解散: 只有群主可以操作; 删 chat_thread 时消息与成员按外键级联清理
	int DismissGroup(long long thread_id, int uid);
	//写入一条群系统消息(msg_type=4), 用于建群、邀请、移除、退群、改名等提示
	bool AddGroupSystemMessage(long long thread_id, int actor, const std::string& content,
		ChatMessageInfo& message);
	//群成员 uid 列表, 用于消息广播与事件推送
	bool GetGroupMemberIds(long long thread_id, std::vector<int>& members);
	//批量写入群消息: 只校验群成员身份, recv_id 固定 0, mentions 是逗号分隔的 @ 名单
	bool SaveGroupMessages(int sender_id, long long thread_id, const std::string& mentions,
		const std::vector<std::pair<std::string, std::string>>& contents,
		std::vector<ChatMessageInfo>& messages);
	//写入一条群资源消息(图片/视频/文件), 资源上传完成后由资源服务器回写状态
	bool SaveGroupResourceMessage(int sender_id, long long thread_id, const std::string& mentions,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	//群已读游标: 只前进不后退, 未读数与会话行"有人@我"都由它推导
	bool MarkGroupRead(int uid, long long thread_id, long long last_message_id);
	//群成员在某个群的单行设置, 不存在返回 false
	bool GetGroupMember(long long thread_id, int uid, GroupMemberInfo& member,
		bool& muted, long long& last_read_message_id);

private:
	//把客户端传入的会话ID解析成有效单聊: 为0时校验好友关系并补建, 失败返回0
	long long EnsureChatThread(int sender_id, int recv_id, long long thread_id);
	//在已有连接上确认群成员身份并取出角色/群昵称/免打扰/已读游标,
	//供建群、邀请、权限判断与消息写入复用, 避免同一事务里重复借连接
	bool GetGroupMemberOn(mysqlx::Session* session, long long thread_id, int uid,
		GroupMemberInfo& member, bool& muted, long long& last_read_message_id);

	//MySQL连接池对象, 用于管理MySQL连接, 提供线程安全的获取和归还连接的接口
	std::unique_ptr<MySqlPool> pool_;
};

//池内的一条连接: 会话对象, 最近活跃时间, 以及是否可复用的标记
class SqlConnection {
public:
	SqlConnection(std::unique_ptr<mysqlx::Session> session, int64_t lasttime) :_session(std::move(session)), _last_oper_time(lasttime) {}
	std::unique_ptr<mysqlx::Session> _session; //X DevAPI会话对象, 使用智能指针管理资源
	int64_t _last_oper_time; //上次操作时间
	bool _healthy{true}; //连接是否可复用, DAO捕获异常时置false, 归还时丢弃并补建
};

//连接池: 对外只暴露借还接口, 由检测线程保活并按池容量补足
//业务线程捕获到异常时把连接标为不可复用, 归还即销毁, 不再进入队列
class MySqlPool {
public:
	MySqlPool(const std::string& host, unsigned port, const std::string& user, const std::string& pass,
		const std::string& schema, int poolSize);

	void checkConnection(); //检查空闲连接, 并把池容量补足到 poolSize_
    bool reconnect(long long timestamp); //重新连接
	std::unique_ptr<SqlConnection> getConnection(); //获取连接
	void returnConnection(std::unique_ptr<SqlConnection> con); //归还连接
	void Close(); //关闭连接池
	~MySqlPool();

private:
	std::string host_; //MySQL服务器地址
	unsigned port_; //X Protocol端口号
	std::string user_; //MySQL用户名
	std::string pass_; //MySQL密码
	std::string schema_; //MySQL数据库名
	int poolSize_; //连接池大小

	std::queue<std::unique_ptr<SqlConnection>> pool_; //连接池队列
	std::mutex mutex_; //用于保护连接队列的互斥锁
	std::condition_variable cond_; //业务线程等待可用连接
	std::condition_variable check_cond_; //检测线程等待心跳周期、补池请求或停池通知
	bool check_requested_{true}; //补池请求标记: 启动时先补齐初始化失败的连接, 之后由丢弃连接的线程置位
	size_t connection_count_{0}; //池内连接总数(空闲+借出), 由mutex_保护; 容量判断不能用队列长度
	std::atomic<bool> b_stop_; //连接池是否停止, 用于控制连接池的生命周期
	std::thread _check_thread; //检测线程, 定期检查连接的状态并保持连接的活跃性(心跳机制)
};
