#pragma once
// 用于管理MySQL连接池并提供MySQL操作接口
//

#include "MysqlDao.h"
#include "Singleton.h"

class MysqlMgr : public Singleton<MysqlMgr>
{
	friend class Singleton<MysqlMgr>; //使基类创建实例时可以访问私有构造函数
public:
	~MysqlMgr(); //使基类析构时可以访问派生类的析构函数

	//注册用户, 返回用户ID, 如果注册失败则返回-1
	int RegUser(const std::string& name, const std::string& email, const std::string& pwd);
	bool CheckEmail(const std::string& name, const std::string& email); //检查用户名和邮箱是否匹配
	bool UpdatePwd(const std::string& name, const std::string& pwd); //更新密码
	//检查邮箱和密码是否匹配, 如果匹配则将用户信息存储在userInfo中
	bool CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo);
	bool AddFriendApply(const int& from, const int& to, const std::string& desc,
		const std::string& back_name, const std::vector<std::string>& tags); //好友申请、留言、备注与标签
	//建立双向好友关系并置申请为已通过; 输出申请方当初给通过方设的备注与标签
	bool AddFriend(const int& from, const int& to, std::string back_name,
		const std::vector<std::string>& tags, std::string& apply_desc,
		std::string& reverse_back, std::vector<std::string>& reverse_tags);

	//更新本人可编辑的资料字段; 昵称与签名的长度由调用方按字符数校验
	bool UpdateUserProfile(int uid, const std::string& nick, int sex, const std::string& desc);
	//修改自己给好友设的备注与标签: 返回 1 已更新, 0 不是好友, -1 数据库错误
	int UpdateFriendRemark(int self_id, int friend_id, const std::string& remark,
		const std::vector<std::string>& tags);

	std::shared_ptr<UserInfo> GetUser(int uid); //根据用户ID获取用户信息
	std::shared_ptr<UserInfo> GetUser(const std::string& name); //根据用户名获取用户信息
	//获取好友申请列表, 根据起始id和限制条数返回列表
	bool GetApplyList(int touid,
		std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit = 10);
	//我发出的好友申请, 与收到的申请分方向展示
	bool GetSentApplyList(int from_uid,
		std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit = 10);
	bool GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info);
	//取某人的好友 uid 列表, 资料变更后按它决定推给谁
	bool GetFriendIds(int uid, std::vector<int>& friend_ids);
	bool MarkChatMessagesRead(int uid, long long thread_id, long long last_message_id,
		bool& changed, int& peer_uid);
	int RecallChatMessage(int uid, long long message_id, ChatMessageInfo& message);
	bool IsFriend(int uid, int touid);
	bool IsMutualFriend(int uid, int touid);
	bool DeleteFriend(int uid, int touid);
	long long GetOrCreatePrivateChat(int user1, int user2); //取单聊会话, 没有则创建
	//分页读取会话列表与消息, 具体语义见 MysqlDao 中同名方法
	bool GetChatThreads(int uid, long long last_thread_id, int limit,
		std::vector<ChatThreadInfo>& threads, bool& has_more);
	bool GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
		std::vector<ChatMessageInfo>& messages, bool& has_more);
	bool SaveChatMessages(int sender_id, int recv_id, long long thread_id,
		const std::vector<std::pair<std::string, std::string>>& contents,
		std::vector<ChatMessageInfo>& messages);
	//图片/视频/文件消息先落库占位, 资源上传完成后由资源服务器回写状态
	bool SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	//资源完成通知按消息ID取类型与内容
	bool GetChatMessage(long long message_id, ChatMessageInfo& message);
	//资源服务器通知: 把指定消息标记为已上传
	bool UpdateChatMessageStatus(long long message_id, int sender, int status);
	//会话类型: 1 单聊 / 2 群聊
	bool GetThreadType(long long thread_id, int& thread_type);
	//群聊的建群、资料、成员与消息, 具体语义见 MysqlDao 中同名方法
	long long CreateGroup(int owner_id, const std::string& group_name,
		const std::vector<int>& members, GroupInfo& info,
		std::vector<GroupMemberInfo>& member_list, int& error);
	bool GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
		std::vector<GroupMemberInfo>& members);
	int UpdateGroupField(long long thread_id, int uid, MysqlDao::GroupField field,
		const std::string& value, std::string& applied);
	int InviteGroupMembers(long long thread_id, int operator_uid,
		const std::vector<int>& members, std::vector<int>& added, int& member_count);
	int RemoveGroupMember(long long thread_id, int operator_uid, int target_uid,
		int& member_count);
	int SetGroupAdmin(long long thread_id, int operator_uid, int target_uid, bool admin);
	int LeaveGroup(long long thread_id, int uid, bool& dismissed);
	int DismissGroup(long long thread_id, int uid);
	bool AddGroupSystemMessage(long long thread_id, int actor, const std::string& content,
		ChatMessageInfo& message);
	bool GetGroupMemberIds(long long thread_id, std::vector<int>& members);
	bool SaveGroupMessages(int sender_id, long long thread_id, const std::string& mentions,
		const std::vector<std::pair<std::string, std::string>>& contents,
		std::vector<ChatMessageInfo>& messages);
	bool SaveGroupResourceMessage(int sender_id, long long thread_id, const std::string& mentions,
		const std::string& client_msg_id, int msg_type, const std::string& content,
		ChatMessageInfo& message);
	bool MarkGroupRead(int uid, long long thread_id, long long last_message_id);
	bool GetGroupMember(long long thread_id, int uid, GroupMemberInfo& member,
		bool& muted, long long& last_read_message_id);

private:
	MysqlMgr();

	MysqlDao _dao; //数据库访问对象
};
