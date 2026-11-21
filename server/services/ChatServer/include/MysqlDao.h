#pragma once
// 数据库访问对象类, 用于封装对MySQL数据库的操作
//

#include "const.h"
#include "Singleton.h"
#include <cstddef>
#include <deque>
#include <unordered_map>
#include "MysqlPool.h"
#include <thread>
#include "data.h"
#include <memory>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <map>
class MysqlSession; //一条MySQL会话（common/MysqlPool.h）
class MysqlPool; //MySQL连接池（common/MysqlPool.h）

class MysqlDao : public Singleton<MysqlDao>
{
	friend class Singleton<MysqlDao>; //基类创建实例时需要访问私有构造函数
public:
	~MysqlDao();
	//检查用户名和密码是否匹配, 如果匹配则将用户信息存储在userInfo中
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
	//双向关系是否完整（两行都在）：发消息的前提
	bool IsMutualFriend(int uid, int touid);
	//删除好友：只删自己这一行，对方仍保留关系
	bool DeleteFriend(int uid, int touid);
	//取单聊会话, 没有则创建; 非好友返回0. 依赖 conversation.dm_key 唯一键去重
	long long GetOrCreatePrivateChat(int user1, int user2);
	//按活跃时间倒序分页取会话列表; last_time 为上一页最后一条的 updated_at（UNIX 秒），
	//旧客户端只传 thread_id 时退回按 thread_id 升序，保证兼容
	//把超过保留期的消息搬到 chat_message_archive 并删除原行，返回搬走的条数。
	//INSERT IGNORE + 按 message_id 顺序分批，两个实例同时跑也只会各自搬不同的批次
	long long ArchiveChatMessages(int retention_days, int batch_size);

	bool GetChatThreads(int uid, long long last_thread_id, int limit,
		std::vector<ChatThreadInfo>& threads, bool& has_more, long long last_time = 0);
	//按消息ID升序分页取会话内消息, 只允许会话成员读取
	bool GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
		std::vector<ChatMessageInfo>& messages, bool& has_more);
	//批量写入消息; thread_id 为0时先找到或创建会话, 相同 (会话,发送者,client_msg_id) 视为重发
	bool SaveChatMessages(int sender_id, int recv_id, long long thread_id,
		const std::vector<ChatContentItem>& contents,
		std::vector<ChatMessageInfo>& messages);
	//写入一条资源消息(图片/视频/文件): 资源还没上传, 先落库占位, 由资源服务器完成后回写状态
	bool SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	//资源上传完成后按消息ID取类型与内容, 用于给接收方拼通知
	bool GetChatMessage(long long message_id, ChatMessageInfo& message);
	//资源服务器通知: 把指定消息的状态改成已上传; sender 用于确认消息归属
	bool UpdateChatMessageStatus(long long message_id, int sender, int upload_state);
	//会话类型: 1 单聊 / 2 群聊; 不存在返回 false
	bool GetThreadType(long long thread_id, int& thread_type);
	//会话类型缓存：业务线程与 gRPC 线程都会读；未命中查库，查询失败按单聊返回
	int GetThreadTypeCached(long long thread_id);

	//建群: 群主 + 勾选的好友(必须是群主的好友), 同时写 conversation/成员与首条系统消息。
	//返回 thread_id, 0 表示失败; error 带回具体原因
	long long CreateGroup(int owner_id, const std::string& group_name,
		const std::vector<int>& members, GroupInfo& info,
		std::vector<GroupMemberInfo>& member_list, int& error);
	//群资料与成员: 只允许群成员读取, 同时带回我的角色、群昵称与免打扰设置
	bool GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
		std::vector<GroupMemberInfo>& members);
	//群设置字段: 与 conversation / conversation_member 的列一一对应
	enum class GroupField { Name = 1, Notice = 2, MyNickname = 3, Muted = 4, Icon = 5 };
	//修改群设置: 群名、公告与群头像需要管理员及以上, 群昵称与免打扰改自己那一行。
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
	//退群: 群主不能直接退群(需先解散)
	int LeaveGroup(long long thread_id, int uid);
	//解散: 只有群主可以操作; 只把 conversation 标记为 state=1, 成员与消息保留
	int DismissGroup(long long thread_id, int uid);
	//写入一条群系统消息(msg_type=4), 用于建群、邀请、移除、退群、改名等提示
	bool AddGroupSystemMessage(long long thread_id, int actor, const std::string& content,
		ChatMessageInfo& message);
	//群成员 uid 列表, 用于消息广播与事件推送
	bool GetGroupMemberIds(long long thread_id, std::vector<int>& members);
	//批量写入群消息: 只校验群成员身份, recv_id 固定 0, mentions 是逗号分隔的 @ 名单
	bool SaveGroupMessages(int sender_id, long long thread_id, const std::string& mentions,
		const std::vector<ChatContentItem>& contents,
		std::vector<ChatMessageInfo>& messages);
	//写入一条群资源消息(图片/视频/文件), 资源上传完成后由资源服务器回写状态
	bool SaveGroupResourceMessage(int sender_id, long long thread_id, const std::string& mentions,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	//群已读游标: 只前进不后退, 未读数与会话行"有人@我"都由它推导
	bool MarkGroupRead(int uid, long long thread_id, long long last_message_id);
	//群成员在某个群的单行设置, 不存在返回 false

private:
	MysqlDao(); //建立连接池；只由 Singleton 调用
	//把客户端传入的会话ID解析成有效单聊: 为0时校验好友关系并补建, 失败返回0
	long long EnsureChatThread(int sender_id, int recv_id, long long thread_id);
	//在已有连接上确认群成员身份并取出角色/群昵称/免打扰/已读游标,
	//供建群、邀请、权限判断与消息写入复用, 避免同一事务里重复借连接
	bool GetGroupMemberOn(MysqlSession* session, long long thread_id, int uid,
		GroupMemberInfo& member, bool& muted, long long& last_read_message_id);

	//MySQL连接池对象, 用于管理MySQL连接, 提供线程安全的获取和归还连接的接口
	//连接池在首次 GetInstance 时建立，进程退出时关闭
	std::unique_ptr<MysqlPool> pool_;

	//会话类型缓存：业务线程与 gRPC 线程都会读，用互斥锁保护；
	//超过上限按插入顺序淘汰最旧的一条，避免长期运行后无限增长
	static constexpr std::size_t kThreadTypeCacheMax = 20000;
	std::mutex _type_mutex;
	std::unordered_map<long long, int> _type_cache;
	std::deque<long long> _type_order;
};
