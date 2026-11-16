#include "MysqlMgr.h"

MysqlMgr::MysqlMgr() {}

MysqlMgr::~MysqlMgr() {}

int MysqlMgr::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	//调用数据库访问对象的RegUser方法, 将注册请求转发给数据库访问对象处理, 返回结果
	return _dao.RegUser(name, email, pwd);
}

bool MysqlMgr::CheckEmail(const std::string& name, const std::string& email)
{
	//调用数据库访问对象的CheckEmail方法, 将检查用户名和邮箱是否匹配的请求转发给数据库访问对象处理
	return _dao.CheckEmail(name, email);
}

bool MysqlMgr::UpdatePwd(const std::string& name, const std::string& pwd)
{
	//调用数据库访问对象的UpdatePwd方法, 将更新密码的请求转发给数据库访问对象处理
	return _dao.UpdatePwd(name, pwd);
}

bool MysqlMgr::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo) {
	//调用数据库访问对象的CheckPwd方法, 将检查邮箱和密码是否匹配的请求转发给数据库访问对象处理
	return _dao.CheckPwd(email, pwd, userInfo);
}

bool MysqlMgr::AddFriendApply(const int& from, const int& to, const std::string& desc,
	const std::string& back_name, const std::vector<std::string>& tags)
{
	//调用数据库访问对象的AddFriendApply方法, 将添加好友申请的请求转发给数据库访问对象处理
	return _dao.AddFriendApply(from, to, desc, back_name, tags);
}

bool MysqlMgr::AddFriend(const int& from, const int& to, std::string back_name,
	const std::vector<std::string>& tags, std::string& apply_desc,
	std::string& reverse_back, std::vector<std::string>& reverse_tags,
	bool& first_approve) {
	return _dao.AddFriend(from, to, back_name, tags, apply_desc, reverse_back,
		reverse_tags, first_approve);
}

bool MysqlMgr::UpdateUserProfile(int uid, const std::string& nick, int sex, const std::string& desc)
{
	return _dao.UpdateUserProfile(uid, nick, sex, desc);
}

int MysqlMgr::UpdateFriendRemark(int self_id, int friend_id, const std::string& remark,
	const std::vector<std::string>& tags)
{
	return _dao.UpdateFriendRemark(self_id, friend_id, remark, tags);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(int uid)
{
	//调用数据库访问对象的GetUser方法, 将根据用户ID获取用户信息的请求转发给数据库访问对象处理
	return _dao.GetUser(uid);
}

std::shared_ptr<UserInfo> MysqlMgr::GetUser(const std::string& name)
{
	//调用数据库访问对象的GetUser方法, 将根据用户名获取用户信息的请求转发给数据库访问对象处理
	return _dao.GetUser(name);
}

bool MysqlMgr::GetApplyList(int touid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	//调用数据库访问对象的GetApplyList方法, 将获取好友申请列表的请求转发给数据库访问对象处理
	return _dao.GetApplyList(touid, applyList, begin, limit);
}

bool MysqlMgr::GetSentApplyList(int from_uid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	return _dao.GetSentApplyList(from_uid, applyList, begin, limit);
}

bool MysqlMgr::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info)
{
	//调用数据库访问对象的GetFriendList方法, 将获取好友列表的请求转发给数据库访问对象处理
	return _dao.GetFriendList(self_id, user_info);
}

bool MysqlMgr::GetFriendIds(int uid, std::vector<int>& friend_ids)
{
	return _dao.GetFriendIds(uid, friend_ids);
}

bool MysqlMgr::MarkChatMessagesRead(int uid, long long thread_id, long long last_message_id,
	bool& changed, int& peer_uid)
{
	return _dao.MarkChatMessagesRead(uid, thread_id, last_message_id, changed, peer_uid);
}

int MysqlMgr::RecallChatMessage(int uid, long long message_id, ChatMessageInfo& message)
{
	return _dao.RecallChatMessage(uid, message_id, message);
}

bool MysqlMgr::IsFriend(int uid, int touid)
{
	return _dao.IsFriend(uid, touid);
}

bool MysqlMgr::IsMutualFriend(int uid, int touid)
{
	return _dao.IsMutualFriend(uid, touid);
}

int MysqlMgr::GetFriendRelation(int me, int peer)
{
	return _dao.GetFriendRelation(me, peer);
}

bool MysqlMgr::DeleteFriend(int uid, int touid)
{
	return _dao.DeleteFriend(uid, touid);
}

long long MysqlMgr::GetOrCreatePrivateChat(int user1, int user2)
{
	//单聊会话由 private_chat 的两人组合唯一确定, 创建过程在DAO里用事务处理
	return _dao.GetOrCreatePrivateChat(user1, user2);
}

bool MysqlMgr::GetChatThreads(int uid, long long last_thread_id, int limit,
	std::vector<ChatThreadInfo>& threads, bool& has_more)
{
	//调用数据库访问对象的GetChatThreads方法, 按会话ID游标向后翻页
	return _dao.GetChatThreads(uid, last_thread_id, limit, threads, has_more);
}

bool MysqlMgr::GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
	std::vector<ChatMessageInfo>& messages, bool& has_more)
{
	//调用数据库访问对象的GetChatMessages方法, 按消息ID游标向后翻页
	return _dao.GetChatMessages(uid, thread_id, last_message_id, limit, messages, has_more);
}

bool MysqlMgr::SaveChatMessages(int sender_id, int recv_id, long long thread_id,
	const std::vector<std::pair<std::string, std::string>>& contents,
	std::vector<ChatMessageInfo>& messages)
{
	//调用数据库访问对象的SaveChatMessages方法, 写入成功时回填消息ID与所属会话
	return _dao.SaveChatMessages(sender_id, recv_id, thread_id, contents, messages);
}

bool MysqlMgr::SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
	const std::string& client_msg_id, int msg_type, const std::string& content,
	ChatMessageInfo& message)
{
	//图片 content 是资源名；视频/文件 content 是带文件名、大小与预览资源的 JSON
	return _dao.SaveResourceMessage(sender_id, recv_id, thread_id, client_msg_id, msg_type,
		content, message);
}

bool MysqlMgr::GetChatMessage(long long message_id, ChatMessageInfo& message)
{
	return _dao.GetChatMessage(message_id, message); //通知接收方前补齐消息类型与内容
}

bool MysqlMgr::UpdateChatMessageStatus(long long message_id, int sender, int status)
{
	return _dao.UpdateChatMessageStatus(message_id, sender, status); //资源完成回调
}

bool MysqlMgr::GetThreadType(long long thread_id, int& thread_type)
{
	return _dao.GetThreadType(thread_id, thread_type);
}

long long MysqlMgr::CreateGroup(int owner_id, const std::string& group_name,
	const std::vector<int>& members, GroupInfo& info,
	std::vector<GroupMemberInfo>& member_list, int& error)
{
	//建群在DAO里用事务写会话、群资料、成员与首条系统消息
	return _dao.CreateGroup(owner_id, group_name, members, info, member_list, error);
}

bool MysqlMgr::GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
	std::vector<GroupMemberInfo>& members)
{
	return _dao.GetGroupInfo(uid, thread_id, info, members);
}

int MysqlMgr::UpdateGroupField(long long thread_id, int uid, MysqlDao::GroupField field,
	const std::string& value, std::string& applied)
{
	return _dao.UpdateGroupField(thread_id, uid, field, value, applied);
}

int MysqlMgr::InviteGroupMembers(long long thread_id, int operator_uid,
	const std::vector<int>& members, std::vector<int>& added, int& member_count)
{
	return _dao.InviteGroupMembers(thread_id, operator_uid, members, added, member_count);
}

int MysqlMgr::RemoveGroupMember(long long thread_id, int operator_uid, int target_uid,
	int& member_count)
{
	return _dao.RemoveGroupMember(thread_id, operator_uid, target_uid, member_count);
}

int MysqlMgr::SetGroupAdmin(long long thread_id, int operator_uid, int target_uid, bool admin)
{
	return _dao.SetGroupAdmin(thread_id, operator_uid, target_uid, admin);
}

int MysqlMgr::LeaveGroup(long long thread_id, int uid, bool& dismissed)
{
	return _dao.LeaveGroup(thread_id, uid, dismissed);
}

int MysqlMgr::DismissGroup(long long thread_id, int uid)
{
	return _dao.DismissGroup(thread_id, uid);
}

bool MysqlMgr::AddGroupSystemMessage(long long thread_id, int actor,
	const std::string& content, ChatMessageInfo& message)
{
	return _dao.AddGroupSystemMessage(thread_id, actor, content, message);
}

bool MysqlMgr::GetGroupMemberIds(long long thread_id, std::vector<int>& members)
{
	return _dao.GetGroupMemberIds(thread_id, members);
}

bool MysqlMgr::SaveGroupMessages(int sender_id, long long thread_id, const std::string& mentions,
	const std::vector<std::pair<std::string, std::string>>& contents,
	std::vector<ChatMessageInfo>& messages)
{
	return _dao.SaveGroupMessages(sender_id, thread_id, mentions, contents, messages);
}

bool MysqlMgr::SaveGroupResourceMessage(int sender_id, long long thread_id,
	const std::string& mentions, const std::string& client_msg_id, int msg_type,
	const std::string& content, ChatMessageInfo& message)
{
	return _dao.SaveGroupResourceMessage(sender_id, thread_id, mentions, client_msg_id,
		msg_type, content, message);
}

bool MysqlMgr::MarkGroupRead(int uid, long long thread_id, long long last_message_id)
{
	return _dao.MarkGroupRead(uid, thread_id, last_message_id);
}

bool MysqlMgr::GetGroupMember(long long thread_id, int uid, GroupMemberInfo& member,
	bool& muted, long long& last_read_message_id)
{
	return _dao.GetGroupMember(thread_id, uid, member, muted, last_read_message_id);
}
