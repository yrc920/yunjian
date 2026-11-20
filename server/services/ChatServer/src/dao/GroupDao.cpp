#include "MysqlDao.h"
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include "Log.h"

//群系统消息的 client_msg_id：同一秒内可能连续写多条（建群 + 邀请），
//用进程内自增序号保证 (thread_id, sender_id, client_msg_id) 唯一键不会撞
namespace {
std::string NextSystemMsgId(long long thread_id)
{
	static std::atomic<unsigned long long> seq{0};
	return "sys_" + std::to_string(thread_id) + "_" + std::to_string(++seq);
}

//群成员资料统一按"角色高的在前、先加入的在前"排列，资料页与消息里的名字都用这一份
bool LoadGroupMembers(MysqlSession& session, long long thread_id,
	std::vector<GroupMemberInfo>& members)
{
	members.clear();
	auto res = session.sql(
		"SELECT cm.uid, u.name, u.nick, u.icon, u.sex, cm.role, cm.nickname, "
		"UNIX_TIMESTAMP(cm.joined_at) "
		"FROM conversation_member cm JOIN user u ON u.uid = cm.uid "
		"WHERE cm.thread_id = ? ORDER BY cm.role DESC, cm.joined_at ASC")
		.bind(thread_id).execute();
	auto row = res.fetchOne();
	while (!row.isNull()) {
		GroupMemberInfo member;
		member.uid = row[0].get<int>();
		member.name = row[1].get<std::string>();
		member.nick = row[2].get<std::string>();
		member.icon = row[3].get<std::string>();
		member.sex = row[4].get<int>();
		member.role = row[5].get<int>();
		member.group_nick = row[6].get<std::string>();
		member.joined_at = row[7].isNull() ? 0 : row[7].get<int64_t>();
		members.push_back(std::move(member));
		row = res.fetchOne();
	}
	return true;
}
}

long long MysqlDao::CreateGroup(int owner_id, const std::string& group_name,
	const std::vector<int>& members, GroupInfo& info,
	std::vector<GroupMemberInfo>& member_list, int& error)
{
	error = ErrorCodes::Success;
	member_list.clear();
	if (owner_id <= 0 || group_name.empty()) {
		error = ErrorCodes::GroupDenied;
		return 0;
	}

	std::vector<int> invite;
	invite.reserve(members.size());
	for (int uid : members) {
		if (uid <= 0 || uid == owner_id)
			continue;
		if (std::find(invite.begin(), invite.end(), uid) == invite.end())
			invite.push_back(uid);
	}
	if (static_cast<int>(invite.size()) + 1 > MAX_GROUP_MEMBER) {
		error = ErrorCodes::GroupFull;
		return 0;
	}

	auto con = pool_->Acquire();
	if (!con) {
		error = ErrorCodes::RPCFailed;
		return 0;
	}
	try {
		for (int uid : invite) {
			auto row = con->sql(
				"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
				"OR (self_id = ? AND friend_id = ?)")
				.bind(owner_id).bind(uid).bind(uid).bind(owner_id).execute().fetchOne();
			if (row.isNull() || row[0].get<int>() != 2) {
				error = ErrorCodes::NotFriend;
				return 0;
			}
		}

		con->startTransaction();
		con->sql(
			"INSERT INTO conversation (thread_type, name, owner_id) VALUES (2, ?, ?)")
			.bind(group_name).bind(owner_id).execute();
		auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->rollback();
			error = ErrorCodes::RPCFailed;
			return 0;
		}
		const long long thread_id = id_row[0].get<int64_t>();
		//群主与成员都写在成员表里，角色 2 表示群主
		con->sql(
			"INSERT INTO conversation_member (thread_id, uid, role) VALUES (?, ?, 2)")
			.bind(thread_id).bind(owner_id).execute();
		for (int uid : invite) {
			con->sql(
				"INSERT INTO conversation_member (thread_id, uid, role) VALUES (?, ?, 0)")
				.bind(thread_id).bind(uid).execute();
		}
		con->commit();

		info.thread_id = thread_id;
		info.name = group_name;
		info.owner_id = owner_id;
		info.member_count = static_cast<int>(invite.size()) + 1;
		info.my_role = static_cast<int>(GroupRole::Owner);
		info.created_at = std::time(nullptr);
		LoadGroupMembers(*con, thread_id, member_list);
		return thread_id;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error creating group: " << e.what();
		error = ErrorCodes::RPCFailed;
		return 0;
	}
}

bool MysqlDao::GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
	std::vector<GroupMemberInfo>& members)
{
	members.clear();
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//JOIN 成员行同时完成"是不是群成员"的校验，非成员查不到任何行
		auto row = con->sql(
			"SELECT c.name, c.notice, c.owner_id, UNIX_TIMESTAMP(c.created_at), "
			"(SELECT COUNT(*) FROM conversation_member g WHERE g.thread_id = c.thread_id), "
			"cm.role, cm.muted, cm.nickname, cm.last_read_message_id "
			"FROM conversation c JOIN conversation_member cm "
			"ON cm.thread_id = c.thread_id AND cm.uid = ? "
			"WHERE c.thread_id = ? AND c.thread_type = 2 AND c.state = 0")
			.bind(uid).bind(thread_id).execute().fetchOne();
		if (row.isNull())
			return false;

		info.thread_id = thread_id;
		info.name = row[0].get<std::string>();
		info.notice = row[1].get<std::string>();
		info.owner_id = row[2].get<int>();
		info.created_at = row[3].isNull() ? 0 : row[3].get<int64_t>();
		info.member_count = row[4].isNull() ? 0 : row[4].get<int>();
		info.my_role = row[5].get<int>();
		info.muted = row[6].get<int>() != 0;
		info.my_nick = row[7].get<std::string>();
		info.last_read_message_id = row[8].isNull() ? 0 : row[8].get<int64_t>();
		LoadGroupMembers(*con, thread_id, members);
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading group info: " << e.what();
		return false;
	}
}

//群设置更新：群名与公告需要管理员及以上；群昵称与免打扰只改自己那一行。
//群昵称走成员表，不碰 user 表，避免"改个群备注把账号昵称也改了"
int MysqlDao::UpdateGroupField(long long thread_id, int uid, GroupField field,
	const std::string& value, std::string& applied)
{
	applied = value;
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		int role = -1;
		bool muted = false;
		long long last_read = 0;
		GroupMemberInfo member;
		if (!GetGroupMemberOn(con.Get(), thread_id, uid, member, muted, last_read))
			return ErrorCodes::GroupNotFound;
		role = member.role;

		if (field == GroupField::Name || field == GroupField::Notice) {
			if (role < static_cast<int>(GroupRole::Admin))
				return ErrorCodes::GroupDenied;
			if (field == GroupField::Name) {
				con->sql("UPDATE conversation SET name = ? "
					"WHERE thread_id = ? AND thread_type = 2")
					.bind(value).bind(thread_id).execute();
			}
			else {
				con->sql("UPDATE conversation SET notice = ? "
					"WHERE thread_id = ? AND thread_type = 2")
					.bind(value).bind(thread_id).execute();
			}
			return ErrorCodes::Success;
		}

		if (field == GroupField::MyNickname) {
			con->sql(
				"UPDATE conversation_member SET nickname = ? WHERE thread_id = ? AND uid = ?")
				.bind(value).bind(thread_id).bind(uid).execute();
			return ErrorCodes::Success;
		}

		const bool target = value == "1";
		con->sql(
			"UPDATE conversation_member SET muted = ? WHERE thread_id = ? AND uid = ?")
			.bind(target ? 1 : 0).bind(thread_id).bind(uid).execute();
		applied = target ? "1" : "0";
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error updating group field: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

//邀请成员：操作者必须是群成员，被邀请人必须是操作者的好友；
//已满 200 人直接拒绝，已在群里的跳过（重复邀请不报错，返回真正加入的人）
int MysqlDao::InviteGroupMembers(long long thread_id, int operator_uid,
	const std::vector<int>& members, std::vector<int>& added, int& member_count)
{
	added.clear();
	member_count = 0;
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con.Get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;

		auto count_row = con->sql(
			"SELECT COUNT(*) FROM conversation_member WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		member_count = count_row.isNull() ? 0 : count_row[0].get<int>();
		if (member_count >= MAX_GROUP_MEMBER)
			return ErrorCodes::GroupFull;

		//先过滤掉已经在群里的，避免把"已存在"当成错误中断整批邀请
		std::vector<int> candidates;
		for (int uid : members) {
			if (uid <= 0 || uid == operator_uid)
				continue;
			if (std::find(candidates.begin(), candidates.end(), uid) != candidates.end())
				continue;
			auto exists = con->sql(
				"SELECT COUNT(*) FROM conversation_member WHERE thread_id = ? AND uid = ?")
				.bind(thread_id).bind(uid).execute().fetchOne();
			if (!exists.isNull() && exists[0].get<int>() > 0)
				continue;
			candidates.push_back(uid);
		}
		if (candidates.empty())
			return ErrorCodes::Success;

		con->startTransaction();
		for (int uid : candidates) {
			auto friend_row = con->sql(
				"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
				"OR (self_id = ? AND friend_id = ?)")
				.bind(operator_uid).bind(uid).bind(uid).bind(operator_uid).execute().fetchOne();
			if (friend_row.isNull() || friend_row[0].get<int>() != 2)
				continue; //不是好友的人静默跳过，由调用方按 added 结果决定提示
			if (member_count >= MAX_GROUP_MEMBER)
				break;
			con->sql(
				"INSERT INTO conversation_member (thread_id, uid, role) VALUES (?, ?, 0)")
				.bind(thread_id).bind(uid).execute();
			added.push_back(uid);
			++member_count;
		}
		if (added.empty()) {
			con->rollback();
			return ErrorCodes::NotFriend;
		}
		con->commit();
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error inviting group members: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

int MysqlDao::RemoveGroupMember(long long thread_id, int operator_uid, int target_uid,
	int& member_count)
{
	member_count = 0;
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		GroupMemberInfo self;
		GroupMemberInfo target;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con.Get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (!GetGroupMemberOn(con.Get(), thread_id, target_uid, target, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (target.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;
		//管理员只能移除普通成员，不能互相踢；群主不受这条限制
		if (self.role == static_cast<int>(GroupRole::Admin)
			&& target.role >= static_cast<int>(GroupRole::Admin))
			return ErrorCodes::GroupDenied;
		if (self.role < static_cast<int>(GroupRole::Admin))
			return ErrorCodes::GroupDenied;

		con->startTransaction();
		con->sql(
			"DELETE FROM conversation_member WHERE thread_id = ? AND uid = ?")
			.bind(thread_id).bind(target_uid).execute();
		auto count_row = con->sql(
			"SELECT COUNT(*) FROM conversation_member WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		member_count = count_row.isNull() ? 0 : count_row[0].get<int>();
		con->commit();
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error removing group member: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

//设/撤管理员只有群主可以操作；目标必须是普通成员或管理员，不能是群主自己
int MysqlDao::SetGroupAdmin(long long thread_id, int operator_uid, int target_uid, bool admin)
{
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		GroupMemberInfo self;
		GroupMemberInfo target;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con.Get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (self.role != static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;
		if (!GetGroupMemberOn(con.Get(), thread_id, target_uid, target, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (target.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;

		const int role = admin ? static_cast<int>(GroupRole::Admin)
			: static_cast<int>(GroupRole::Member);
		con->sql(
			"UPDATE conversation_member SET role = ? WHERE thread_id = ? AND uid = ?")
			.bind(role).bind(thread_id).bind(target_uid).execute();
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error setting group admin: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

//退群：群主不能直接退，必须先解散或（后续版本）转让；普通成员与管理员删掉自己那一行
int MysqlDao::LeaveGroup(long long thread_id, int uid)
{
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con.Get(), thread_id, uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (self.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;

		con->startTransaction();
		con->sql("DELETE FROM conversation_member WHERE thread_id = ? AND uid = ?")
			.bind(thread_id).bind(uid).execute();
		con->commit();
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error leaving group: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

//解散：只把 conversation 标记为 state=1，成员与消息保留，便于审计与历史查看
int MysqlDao::DismissGroup(long long thread_id, int uid)
{
	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;
	try {
		auto row = con->sql(
			"SELECT owner_id FROM conversation WHERE thread_id = ? AND thread_type = 2")
			.bind(thread_id).execute().fetchOne();
		if (row.isNull())
			return ErrorCodes::GroupNotFound;
		if (row[0].get<int>() != uid)
			return ErrorCodes::GroupDenied;

		//软删：只打标记，成员与消息都留着，供审计与保留策略处理；
		//直接删会级联掉消息，群解散后历史就再也查不到了
		con->sql(
			"UPDATE conversation SET state = 1, dissolved_at = NOW() "
			"WHERE thread_id = ? AND state = 0")
			.bind(thread_id).execute();
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error dismissing group: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

bool MysqlDao::AddGroupSystemMessage(long long thread_id, int actor,
	const std::string& content, ChatMessageInfo& message)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		con->startTransaction();
		con->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
			"msg_type, state, upload_state, mentions) VALUES (?, ?, 0, ?, ?, 4, 0, 1, '')")
			.bind(thread_id).bind(actor).bind(NextSystemMsgId(thread_id)).bind(content).execute();
		auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		auto time_row = con->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		con->commit();

		//系统消息同样推进会话排序；失败只记日志，不影响已经写入的消息
		const long long created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		try {
			con->sql(
				"UPDATE conversation SET last_message_id = ?, last_message_at = FROM_UNIXTIME(?) "
				"WHERE thread_id = ? AND last_message_id < ?")
				.bind(static_cast<int64_t>(message_id)).bind(created_at)
				.bind(thread_id).bind(static_cast<int64_t>(message_id)).execute();
		}
		catch (MysqlError& e) {
			LOG_ERROR << "mysql error updating conversation time: " << e.what();
		}

		message.message_id = message_id;
		message.thread_id = thread_id;
		message.sender_id = actor;
		message.recv_id = 0;
		message.content = content;
		message.msg_type = 4;
		message.created_at = created_at;
		return true;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error adding group system message: " << e.what();
		return false;
	}
}

bool MysqlDao::GetGroupMemberIds(long long thread_id, std::vector<int>& members)
{
	members.clear();
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		auto res = con->sql(
			"SELECT uid FROM conversation_member WHERE thread_id = ?")
			.bind(thread_id).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			members.push_back(row[0].get<int>());
			row = res.fetchOne();
		}
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading group members: " << e.what();
		return false;
	}
}

bool MysqlDao::SaveGroupMessages(int sender_id, long long thread_id, const std::string& mentions,
	const std::vector<std::pair<std::string, std::string>>& contents,
	std::vector<ChatMessageInfo>& messages)
{
	messages.clear();
	if (contents.empty() || thread_id <= 0)
		return true;
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//成员 + 群类型一次校验，非成员或单聊 thread 都拿不到行
		auto member = con->sql(
			"SELECT 1 FROM conversation_member cm JOIN conversation c "
			"ON c.thread_id = cm.thread_id AND c.thread_type = 2 AND c.state = 0 "
			"WHERE cm.thread_id = ? AND cm.uid = ?")
			.bind(thread_id).bind(sender_id).execute().fetchOne();
		if (member.isNull())
			return false;

		//整批消息一个事务：幂等键与单聊一致，(thread_id, sender_id, client_msg_id) 唯一
		con->startTransaction();

		//state/upload_state 用默认值 0/1，文本消息没有资源上传状态
		std::string insert_sql =
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
			"msg_type, mentions) VALUES ";
		for (std::size_t i = 0; i < contents.size(); ++i) {
			if (i > 0) insert_sql += ',';
			insert_sql += "(?, ?, 0, ?, ?, 0, ?)";
		}
		insert_sql += " ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)";
		auto insert = con->sql(insert_sql);
		for (const auto& item : contents)
			insert.bind(thread_id).bind(sender_id).bind(item.first)
				.bind(item.second).bind(mentions);
		insert.execute();

		//与单聊一致：按客户端消息ID一次取回 id 与落库时间
		std::string select_sql =
			"SELECT client_msg_id, message_id, UNIX_TIMESTAMP(created_at) FROM chat_message "
			"WHERE thread_id = ? AND sender_id = ? AND client_msg_id IN (";
		for (std::size_t i = 0; i < contents.size(); ++i) {
			if (i > 0) select_sql += ',';
			select_sql += '?';
		}
		select_sql += ')';
		auto select = con->sql(select_sql);
		select.bind(thread_id).bind(sender_id);
		for (const auto& item : contents)
			select.bind(item.first);
		auto res = select.execute();

		std::unordered_map<std::string, std::pair<long long, long long>> saved;
		auto row = res.fetchOne();
		while (!row.isNull()) {
			saved.emplace(row[0].get<std::string>(),
				std::make_pair(row[1].get<int64_t>(),
					row[2].isNull() ? 0 : row[2].get<int64_t>()));
			row = res.fetchOne();
		}
		for (const auto& item : contents) {
			auto iter = saved.find(item.first);
			if (iter == saved.end()) {
				con->rollback();
				return false;
			}
			ChatMessageInfo info;
			info.message_id = iter->second.first;
			info.thread_id = thread_id;
			info.sender_id = sender_id;
			info.recv_id = 0;
			info.client_msg_id = item.first;
			info.content = item.second;
			info.msg_type = 0;
			info.status = 1;
			info.mentions = mentions;
			info.created_at = iter->second.second;
			messages.push_back(std::move(info));
		}

		con->commit();

		//与会话列表排序解耦，避免并发发送时在会话行上形成锁顺序问题
		long long newest_id = 0;
		long long newest_time = 0;
		for (const auto& message : messages) {
			if (message.message_id > newest_id) {
				newest_id = message.message_id;
				newest_time = message.created_at;
			}
		}
		try {
			con->sql(
				"UPDATE conversation SET last_message_id = ?, last_message_at = FROM_UNIXTIME(?) "
				"WHERE thread_id = ? AND last_message_id < ?")
				.bind(static_cast<int64_t>(newest_id)).bind(newest_time)
				.bind(thread_id).bind(static_cast<int64_t>(newest_id)).execute();
		}
		catch (MysqlError& e) {
			LOG_ERROR << "mysql error updating conversation time: " << e.what();
		}
		return true;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error saving group messages: " << e.what();
		return false;
	}
}

bool MysqlDao::SaveGroupResourceMessage(int sender_id, long long thread_id,
	const std::string& mentions, const std::string& client_msg_id, int msg_type,
	const std::string& content, ChatMessageInfo& message)
{
	if (msg_type < 1 || msg_type > 3)
		return false;
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		auto member = con->sql(
			"SELECT 1 FROM conversation_member cm JOIN conversation c "
			"ON c.thread_id = cm.thread_id AND c.thread_type = 2 AND c.state = 0 "
			"WHERE cm.thread_id = ? AND cm.uid = ?")
			.bind(thread_id).bind(sender_id).execute().fetchOne();
		if (member.isNull())
			return false;

		con->startTransaction();
		con->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
			"msg_type, upload_state, mentions) VALUES (?, ?, 0, ?, ?, ?, 0, ?) "
			"ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
			.bind(thread_id).bind(sender_id).bind(client_msg_id).bind(content)
			.bind(msg_type).bind(mentions).execute();
		auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		auto time_row = con->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		con->commit();

		const long long created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		try {
			con->sql(
				"UPDATE conversation SET last_message_id = ?, last_message_at = FROM_UNIXTIME(?) "
				"WHERE thread_id = ? AND last_message_id < ?")
				.bind(static_cast<int64_t>(message_id)).bind(created_at)
				.bind(thread_id).bind(static_cast<int64_t>(message_id)).execute();
		}
		catch (MysqlError& e) {
			LOG_ERROR << "mysql error updating conversation time: " << e.what();
		}

		message.message_id = message_id;
		message.thread_id = thread_id;
		message.sender_id = sender_id;
		message.recv_id = 0;
		message.client_msg_id = client_msg_id;
		message.content = content;
		message.msg_type = msg_type;
		message.status = 0;
		message.created_at = created_at;
		message.mentions = mentions;
		return true;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error saving group resource message: " << e.what();
		return false;
	}
}

bool MysqlDao::MarkGroupRead(int uid, long long thread_id, long long last_message_id)
{
	if (uid <= 0 || thread_id <= 0 || last_message_id <= 0)
		return false;
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//GREATEST 保证游标只前进：乱序到达的旧"已读"不会把未读数又拉回来
		auto result = con->sql(
			"UPDATE conversation_member SET last_read_message_id = "
			"GREATEST(last_read_message_id, ?) WHERE thread_id = ? AND uid = ?")
			.bind(static_cast<int64_t>(last_message_id)).bind(thread_id).bind(uid).execute();
		if (result.getAffectedItemsCount() > 0)
			return true;
		//值没变化时 affected 为 0，仍要确认是群成员，不能把"不是成员"当成成功
		GroupMemberInfo member;
		bool muted = false;
		long long last_read = 0;
		return GetGroupMemberOn(con.Get(), thread_id, uid, member, muted, last_read);
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error marking group read: " << e.what();
		return false;
	}
}

//同一连接内复用：群资料、权限判断、消息写入都要先确认"我是不是这个群的成员"
bool MysqlDao::GetGroupMemberOn(MysqlSession* session, long long thread_id, int uid,
	GroupMemberInfo& member, bool& muted, long long& last_read_message_id)
{
	muted = false;
	last_read_message_id = 0;
	auto row = session->sql(
		"SELECT cm.uid, u.name, u.nick, u.icon, u.sex, cm.role, cm.nickname, "
		"cm.muted, cm.last_read_message_id, UNIX_TIMESTAMP(cm.joined_at) "
		"FROM conversation_member cm "
		"JOIN conversation c ON c.thread_id = cm.thread_id AND c.thread_type = 2 "
		"  AND c.state = 0 "
		"JOIN user u ON u.uid = cm.uid "
		"WHERE cm.thread_id = ? AND cm.uid = ?")
		.bind(thread_id).bind(uid).execute().fetchOne();
	if (row.isNull())
		return false;
	member.uid = row[0].get<int>();
	member.name = row[1].get<std::string>();
	member.nick = row[2].get<std::string>();
	member.icon = row[3].get<std::string>();
	member.sex = row[4].get<int>();
	member.role = row[5].get<int>();
	member.group_nick = row[6].get<std::string>();
	muted = row[7].get<int>() != 0;
	last_read_message_id = row[8].isNull() ? 0 : row[8].get<int64_t>();
	member.joined_at = row[9].isNull() ? 0 : row[9].get<int64_t>();
	return true;
}
