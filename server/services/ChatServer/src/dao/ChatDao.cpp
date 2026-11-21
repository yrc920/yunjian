#include "MysqlDao.h"
#include <algorithm>
#include <unordered_map>
#include "Log.h"

bool MysqlDao::MarkChatMessagesRead(int uid, long long thread_id, long long last_message_id,
	bool& changed, int& peer_uid)
{
	changed = false;
	peer_uid = 0;
	if (uid <= 0 || thread_id <= 0 || last_message_id <= 0)
		return false;

	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//单聊才有"对端已读"回执；成员行同时完成归属校验
		auto member = con->sql(
			"SELECT cm2.uid FROM conversation_member cm2 "
			"JOIN conversation c ON c.thread_id = cm2.thread_id "
			"WHERE cm2.thread_id = ? AND cm2.uid <> ? AND c.thread_type = 1 LIMIT 1")
			.bind(thread_id).bind(uid).execute().fetchOne();
		if (member.isNull())
			return false;
		peer_uid = member[0].get<int>();

		//已读位置只前进不后退：重复上报或乱序上报都不会把游标改小
		auto result = con->sql(
			"UPDATE conversation_member SET last_read_message_id = ? "
			"WHERE thread_id = ? AND uid = ? AND last_read_message_id < ?")
			.bind(static_cast<int64_t>(last_message_id)).bind(thread_id).bind(uid)
			.bind(static_cast<int64_t>(last_message_id)).execute();
		changed = result.getAffectedItemsCount() > 0;
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error marking read: " << e.what();
		return false;
	}
}

//撤回自己发的消息：只允许发送者本人、2 分钟内、未撤回。
//撤回不物理删除，改为 state=1 并清空正文，双方拉历史时都只能看到提示
int MysqlDao::RecallChatMessage(int uid, long long message_id, ChatMessageInfo& message)
{
	if (uid <= 0 || message_id <= 0)
		return ErrorCodes::RecallDenied;

	auto con = pool_->Acquire();
	if (!con)
		return ErrorCodes::RPCFailed;


	try {
		auto row = con->sql(
			"SELECT thread_id, sender_id, recv_id, client_msg_id, UNIX_TIMESTAMP(created_at), state "
			"FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		if (row.isNull() || row[1].get<int>() != uid)
			return ErrorCodes::RecallDenied;
		if (row[5].get<int>() != 0)
			return ErrorCodes::RecallDenied;

		const long long created_at = row[4].get<int64_t>();
		if (std::time(nullptr) - created_at > RECALL_WINDOW_SECONDS)
			return ErrorCodes::RecallExpired;

		con->sql(
			"UPDATE chat_message SET state = 1, content = '', msg_type = 0 "
			"WHERE message_id = ? AND state = 0")
			.bind(static_cast<int64_t>(message_id)).execute();

		message.message_id = message_id;
		message.thread_id = row[0].get<int64_t>();
		message.sender_id = uid;
		message.recv_id = row[2].get<int>();
		message.client_msg_id = row[3].get<std::string>();
		message.content.clear();
		message.msg_type = 0;
		message.created_at = created_at;
		message.is_read = false; //已读状态改由成员游标表示，撤回通知不需要它
		message.recalled = true;
		return ErrorCodes::Success;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error recalling message: " << e.what();
		return ErrorCodes::RPCFailed;
	}
}

long long MysqlDao::GetOrCreatePrivateChat(int user1, int user2)
{
	//会话只对好友开放; 自己和自己不建会话
	if (user1 <= 0 || user2 <= 0 || user1 == user2)
		return 0;
	//单聊用 dm_key 去重：两人按固定顺序拼成一个键，天然避免 (A,B)/(B,A) 两条会话
	const std::string dm_key = std::to_string(std::min(user1, user2)) + ":"
		+ std::to_string(std::max(user1, user2));

	//会话通常已经存在: 先用普通查询取一次, 不开事务, 也不会给不存在的行加锁
	{
		auto con = pool_->Acquire();
		if (!con)
			return 0;
		try {
			auto friendship = con->sql(
				"SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ?")
				.bind(user1).bind(user2).execute().fetchOne();
			if (friendship.isNull())
				return 0;

			auto existing = con->sql(
				"SELECT thread_id FROM conversation WHERE dm_key = ?")
				.bind(dm_key).execute().fetchOne();
			if (!existing.isNull())
				return existing[0].get<int64_t>();
		}
		catch (MysqlError& e) {
			LOG_ERROR << "mysql error reading private chat: " << e.what();
			return 0;
		}
	}

	//没有会话时直接插入，由 uk_conversation_dm 唯一键决定谁创建成功：
	//不要先 SELECT ... FOR UPDATE 占位，查不到时的间隙锁会和 INSERT 形成死锁
	for (int attempt = 0; attempt < 3; ++attempt) {
		auto con = pool_->Acquire();
		if (!con)
			return 0;
		try {
			con->startTransaction();
			con->sql(
				"INSERT INTO conversation (thread_type, dm_key) VALUES (1, ?)")
				.bind(dm_key).execute();
			auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
			if (id_row.isNull()) {
				con->rollback();
				return 0;
			}
			const long long thread_id = id_row[0].get<int64_t>();
			//单聊两人各占一行成员记录，未读与已读游标都由这张表承载
			con->sql(
				"INSERT INTO conversation_member (thread_id, uid) VALUES (?, ?), (?, ?)")
				.bind(thread_id).bind(std::min(user1, user2))
				.bind(thread_id).bind(std::max(user1, user2)).execute();
			con->commit();
			return thread_id;
		}
		catch (MysqlError& e) {
			try { con->rollback(); }
			catch (...) {}
			//并发下另一个请求先建成功：回滚后读它的 thread_id 直接复用
			try {
				auto again = con->sql(
					"SELECT thread_id FROM conversation WHERE dm_key = ?")
					.bind(dm_key).execute().fetchOne();
				if (!again.isNull())
					return again[0].get<int64_t>();
			}
			catch (...) {}
			LOG_ERROR << "mysql error creating private chat: " << e.what();
		}
	}
	return 0;
}

long long MysqlDao::ArchiveChatMessages(int retention_days, int batch_size)
{
	if (retention_days < 0 || batch_size <= 0)
		return -1;

	auto con = pool_->Acquire();
	if (!con)
		return -1;
	long long total = 0;
	try {
		for (;;) {
			con->startTransaction();

			//先搬后删，两步用同一个截止时间与排序，批边界稳定；
			//INSERT IGNORE 让重复归档（另一实例先搬过）变成无操作
			con->sql(
				"INSERT IGNORE INTO chat_message_archive "
				"(message_id, thread_id, sender_id, recv_id, client_msg_id, content, "
				"msg_type, state, upload_state, mentions, created_at) "
				"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, "
				"msg_type, state, upload_state, mentions, created_at FROM chat_message "
				"WHERE created_at < DATE_SUB(NOW(), INTERVAL ? DAY) "
				"ORDER BY message_id LIMIT ?")
				.bind(retention_days).bind(batch_size).execute();

			auto removed = con->sql(
				"DELETE FROM chat_message WHERE created_at < DATE_SUB(NOW(), INTERVAL ? DAY) "
				"ORDER BY message_id LIMIT ?")
				.bind(retention_days).bind(batch_size).execute()
				.getAffectedItemsCount();

			con->commit();
			total += static_cast<long long>(removed);
			if (static_cast<int>(removed) < batch_size)
				break; //最后一批不满，说明已没有更早的消息
		}
		return total;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error archiving messages: " << e.what();
		return -1;
	}
}

bool MysqlDao::GetChatThreads(int uid, long long last_thread_id, int limit,
	std::vector<ChatThreadInfo>& threads, bool& has_more, long long last_time)
{
	threads.clear();
	has_more = false;
	//单页上限固定为100, 防止客户端传过大的值
	limit = std::clamp(limit, 1, 100);
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//成员表同时完成"我在这个会话里"的过滤；单聊对端、未读数、@我 都按
		//conversation_member.last_read_message_id 计算，不再逐条统计 is_read
		const std::string base_sql =
			"SELECT c.thread_id, c.thread_type, UNIX_TIMESTAMP(c.last_message_at), "
			"(c.thread_type = 1), "
			"CASE WHEN c.thread_type = 1 THEN "
			"  (SELECT p.uid FROM conversation_member p "
			"    WHERE p.thread_id = c.thread_id AND p.uid <> ? LIMIT 1) ELSE 0 END, "
			"u.name, u.nick, u.icon, u.sex, "
			"c.name, c.owner_id, cm.role, cm.nickname, cm.muted, "
			"(SELECT COUNT(*) FROM conversation_member g WHERE g.thread_id = c.thread_id), "
			"(SELECT COUNT(*) FROM chat_message m WHERE m.thread_id = c.thread_id "
			"   AND m.message_id > cm.last_read_message_id AND m.sender_id <> ?), "
			"(c.thread_type = 2 AND EXISTS (SELECT 1 FROM chat_message m2 "
			"   WHERE m2.thread_id = c.thread_id AND m2.message_id > cm.last_read_message_id "
			"   AND m2.sender_id <> ? AND FIND_IN_SET(?, m2.mentions))), c.icon, c.icon_owner "
			"FROM conversation c "
			"JOIN conversation_member cm ON cm.thread_id = c.thread_id AND cm.uid = ? "
			"LEFT JOIN user u ON u.uid = CASE WHEN c.thread_type = 1 THEN "
			"  (SELECT p2.uid FROM conversation_member p2 "
			"    WHERE p2.thread_id = c.thread_id AND p2.uid <> ? LIMIT 1) ELSE 0 END "
			"WHERE c.state = 0"; //已解散的群不再出现在会话列表里

		//前 6 个参数是 SELECT / JOIN / 成员过滤里的 uid，三种游标分支共用
		auto bind_common = [&](MysqlStatement& stmt) {
			stmt.bind(uid).bind(uid).bind(uid).bind(std::to_string(uid)).bind(uid).bind(uid);
		};
		auto parse_rows = [&](MysqlResult& res) {
			auto row = res.fetchOne();
			while (!row.isNull()) {
				if (static_cast<int>(threads.size()) == limit) {
					has_more = true;
					break;
				}
				ChatThreadInfo info;
				info.thread_id = row[0].get<int64_t>();
				info.thread_type = row[1].get<int>();
				info.updated_at = row[2].isNull() ? 0 : row[2].get<int64_t>();
				if (row[3].get<int>() != 0) {
					info.thread_type = static_cast<int>(ThreadType::Private);
					info.peer_uid = row[4].isNull() ? 0 : row[4].get<int>();
					info.peer_name = row[5].isNull() ? "" : row[5].get<std::string>();
					info.peer_nick = row[6].isNull() ? "" : row[6].get<std::string>();
					info.peer_icon = row[7].isNull() ? "" : row[7].get<std::string>();
					info.peer_sex = row[8].isNull() ? 0 : row[8].get<int>();
				}
				else {
					info.thread_type = static_cast<int>(ThreadType::Group);
					info.group_name = row[9].isNull() ? "" : row[9].get<std::string>();
					info.group_owner = row[10].isNull() ? 0 : row[10].get<int>();
					info.my_role = row[11].isNull() ? 0 : row[11].get<int>();
					info.muted = !row[13].isNull() && row[13].get<int>() != 0;
					info.group_member_count = row[14].isNull() ? 0 : row[14].get<int>();
					info.group_icon = row[17].isNull() ? "" : row[17].get<std::string>();
					info.group_icon_owner = row[18].isNull() ? 0 : row[18].get<int>();
				}
				info.unread = row[15].isNull() ? 0 : row[15].get<int>();
				info.at_me = !row[16].isNull() && row[16].get<int>() != 0;
				threads.push_back(std::move(info));
				row = res.fetchOne();
			}
		};

		if (last_time > 0) {
			//复合游标：同一秒内按 thread_id 继续向前，避免 ordering tie 造成重复或漏项
			const std::string sql = base_sql +
				" AND (c.last_message_at < FROM_UNIXTIME(?) "
				" OR (c.last_message_at = FROM_UNIXTIME(?) AND c.thread_id < ?)) "
				"ORDER BY c.last_message_at DESC, c.thread_id DESC LIMIT ?";
			auto stmt = con->sql(sql);
			bind_common(stmt);
			auto res = stmt.bind(last_time).bind(last_time).bind(last_thread_id)
				.bind(limit + 1).execute();
			parse_rows(res);
		}
		else if (last_thread_id > 0) {
			//旧客户端的 thread_id 游标：保持原来的升序语义
			const std::string sql = base_sql +
				" AND c.thread_id > ? ORDER BY c.thread_id ASC LIMIT ?";
			auto stmt = con->sql(sql);
			bind_common(stmt);
			auto res = stmt.bind(last_thread_id).bind(limit + 1).execute();
			parse_rows(res);
		}
		else {
			const std::string sql = base_sql +
				" ORDER BY c.last_message_at DESC, c.thread_id DESC LIMIT ?";
			auto stmt = con->sql(sql);
			bind_common(stmt);
			auto res = stmt.bind(limit + 1).execute();
			parse_rows(res);
		}
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading chat threads: " << e.what();
		return false;
	}
}

bool MysqlDao::GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
	std::vector<ChatMessageInfo>& messages, bool& has_more)
{
	messages.clear();
	has_more = false;
	limit = std::clamp(limit, 1, 100);
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//成员校验：不是会话成员拿不到任何历史，也防止用别人的 thread_id 翻记录
		auto member = con->sql(
			"SELECT 1 FROM conversation_member WHERE thread_id = ? AND uid = ?")
			.bind(thread_id).bind(uid).execute().fetchOne();
		if (member.isNull())
			return false;

		//单聊带出对端的已读游标，客户端据此把单勾改成双勾；群聊不逐条回执
		auto res = con->sql(
			"SELECT m.message_id, m.sender_id, m.recv_id, m.client_msg_id, m.content, "
			"UNIX_TIMESTAMP(m.created_at), m.msg_type, m.state, m.upload_state, m.mentions, "
			"(peer.last_read_message_id >= m.message_id) "
			"FROM chat_message m "
			"JOIN conversation c ON c.thread_id = m.thread_id "
			"LEFT JOIN conversation_member peer ON peer.thread_id = m.thread_id "
			"  AND peer.uid <> ? AND c.thread_type = 1 "
			"WHERE m.thread_id = ? AND m.message_id > ? "
			"ORDER BY m.message_id ASC LIMIT ?")
			.bind(uid).bind(thread_id).bind(static_cast<int64_t>(last_message_id))
			.bind(limit + 1).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			if (static_cast<int>(messages.size()) == limit) {
				has_more = true;
				break;
			}
			ChatMessageInfo info;
			info.message_id = row[0].get<int64_t>();
			info.thread_id = thread_id;
			info.sender_id = row[1].get<int>();
			info.recv_id = row[2].get<int>();
			info.client_msg_id = row[3].get<std::string>();
			info.content = row[4].get<std::string>();
			info.created_at = row[5].isNull() ? 0 : row[5].get<int64_t>();
			info.msg_type = row[6].get<int>();
			info.recalled = row[7].get<int>() != 0;
			info.status = row[8].get<int>();
			info.mentions = row[9].get<std::string>();
			info.is_read = row[10].get<int>() != 0;
			messages.push_back(std::move(info));
			row = res.fetchOne();
		}
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading chat messages: " << e.what();
		return false;
	}
}

long long MysqlDao::EnsureChatThread(int sender_id, int recv_id, long long thread_id)
{
	//已有会话直接返回，归属与好友关系由调用方在写库事务前一次校验；
	//只有客户端还没拿到 thread_id 时才建会话
	if (thread_id > 0)
		return thread_id;
	return GetOrCreatePrivateChat(sender_id, recv_id);
}

bool MysqlDao::SaveChatMessages(int sender_id, int recv_id, long long thread_id,
	const std::vector<ChatContentItem>& contents,
	std::vector<ChatMessageInfo>& messages)
{
	messages.clear();
	if (contents.empty())
		return true;
	//客户端还没拿到会话ID时才走创建路径；正常聊天都带 thread_id
	if (thread_id <= 0) {
		thread_id = EnsureChatThread(sender_id, recv_id, 0);
		if (thread_id <= 0)
			return false;
	}
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//一次查询同时确认两件事：会话属于这两个人、双向好友关系都还在。
		//原实现先查好友、再查归属，要借两次连接、多两次往返
		const int low = std::min(sender_id, recv_id);
		const int high = std::max(sender_id, recv_id);
		auto member = con->sql(
			"SELECT 1 FROM conversation_member cm "
			"JOIN conversation c ON c.thread_id = cm.thread_id "
			"WHERE cm.thread_id = ? AND cm.uid = ? AND c.thread_type = 1 "
			"AND EXISTS (SELECT 1 FROM friend f1 WHERE f1.self_id = ? AND f1.friend_id = ?) "
			"AND EXISTS (SELECT 1 FROM friend f2 WHERE f2.self_id = ? AND f2.friend_id = ?)")
			.bind(thread_id).bind(sender_id).bind(low).bind(high)
			.bind(high).bind(low)
			.execute().fetchOne();
		if (member.isNull())
			return false;

		//整批消息一个事务：要么全部拿到 message_id, 要么一条都不落库
		con->startTransaction();

		//一条语句写入整批；同一条重发时保留库里原来的 message_id
		std::string insert_sql =
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, msg_type) "
			"VALUES ";
		for (std::size_t i = 0; i < contents.size(); ++i) {
			if (i > 0) insert_sql += ',';
			insert_sql += "(?, ?, ?, ?, ?, ?)";
		}
		insert_sql += " ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)";
		auto insert = con->sql(insert_sql);
		for (const auto& item : contents)
			insert.bind(thread_id).bind(sender_id).bind(recv_id)
				.bind(item.client_msg_id).bind(item.content).bind(item.msg_type);
		insert.execute();

		//批量插入不能用 LAST_INSERT_ID 推断 id（自增只保证单条语句内连续），
		//按客户端消息ID一次取回 message_id 与落库时间；重发时拿到的也是原值
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
			select.bind(item.client_msg_id);
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
			auto iter = saved.find(item.client_msg_id);
			if (iter == saved.end()) { //写入被忽略或读取失败，整批回滚
				con->rollback();
				return false;
			}
			ChatMessageInfo info;
			info.message_id = iter->second.first;
			info.thread_id = thread_id;
			info.sender_id = sender_id;
			info.recv_id = recv_id;
			info.client_msg_id = item.client_msg_id;
			info.content = item.content;
			info.msg_type = item.msg_type;
			info.created_at = iter->second.second;
			messages.push_back(std::move(info));
		}

		con->commit();

		//会话的最后消息只影响列表排序，放在事务外：不让同一会话的发送者在会话行上排队，
		//也避免"插入消息 + 更新会话行"的锁顺序在并发发送时形成死锁。
		//只有更新的消息才覆盖，乱序提交不会把游标退回去
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
		LOG_ERROR << "mysql error saving chat messages: " << e.what();
		return false;
	}
}

bool MysqlDao::SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
	const std::string& client_msg_id, int msg_type, const std::string& content,
	ChatMessageInfo& message)
{
	if (msg_type < 1 || msg_type > 3)
		return false;
	//没有会话时先建；正常聊天都带 thread_id
	if (thread_id <= 0) {
		thread_id = EnsureChatThread(sender_id, recv_id, 0);
		if (thread_id <= 0)
			return false;
	}

	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//与文本消息一样，一次查询同时校验会话归属与双向好友关系
		const int low = std::min(sender_id, recv_id);
		const int high = std::max(sender_id, recv_id);
		auto member = con->sql(
			"SELECT 1 FROM conversation_member cm "
			"JOIN conversation c ON c.thread_id = cm.thread_id "
			"WHERE cm.thread_id = ? AND cm.uid = ? AND c.thread_type = 1 "
			"AND EXISTS (SELECT 1 FROM friend f1 WHERE f1.self_id = ? AND f1.friend_id = ?) "
			"AND EXISTS (SELECT 1 FROM friend f2 WHERE f2.self_id = ? AND f2.friend_id = ?)")
			.bind(thread_id).bind(sender_id).bind(low).bind(high)
			.bind(high).bind(low)
			.execute().fetchOne();
		if (member.isNull())
			return false;

		con->startTransaction();
		//upload_state=0 表示资源还没上传完成, 由后续资源回调改成已就绪；
		//content 对图片是资源名, 对视频/文件是文件名、大小、资源名与预览资源的 JSON
		con->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, msg_type, upload_state) "
			"VALUES (?, ?, ?, ?, ?, ?, 0) ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
			.bind(thread_id).bind(sender_id).bind(recv_id).bind(client_msg_id).bind(content)
			.bind(msg_type).execute();
		auto id_row = con->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		//重发同一条时库里保留原始时间, 这里按 message_id 读回来, 不用本地时间冒充
		auto time_row = con->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		const long long created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		con->commit();

		//会话时间放在事务外，避免与并发文本消息在会话行上形成锁顺序问题
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
		message.recv_id = recv_id;
		message.client_msg_id = client_msg_id;
		message.content = content;
		message.msg_type = msg_type;
		message.status = 0;
		message.created_at = created_at;
		return true;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error saving resource message: " << e.what();
		return false;
	}
}

bool MysqlDao::GetChatMessage(long long message_id, ChatMessageInfo& message)
{
	if (message_id <= 0)
		return false;

	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		auto row = con->sql(
			"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, "
			"upload_state, msg_type, UNIX_TIMESTAMP(created_at), state "
			"FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		if (row.isNull())
			return false;
		message.message_id = row[0].get<int64_t>();
		message.thread_id = row[1].get<int64_t>();
		message.sender_id = row[2].get<int>();
		message.recv_id = row[3].get<int>();
		message.client_msg_id = row[4].get<std::string>();
		message.content = row[5].get<std::string>();
		message.status = row[6].get<int>();
		message.msg_type = row[7].get<int>();
		message.created_at = row[8].get<int64_t>();
		message.is_read = false; //逐条已读已被成员游标取代，通知路径不依赖它
		message.recalled = row[9].get<int>() != 0;
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error getting chat message: " << e.what();
		return false;
	}
}


bool MysqlDao::UpdateChatMessageStatus(long long message_id, int sender, int upload_state)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		//带上 sender 限定，避免拿别人的 message_id 改状态；重复回调affected rows为0仍按成功
		con->sql("UPDATE chat_message SET upload_state = ? WHERE message_id = ? AND sender_id = ?")
			.bind(upload_state).bind(static_cast<int64_t>(message_id)).bind(sender).execute();
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error updating chat message status: " << e.what();
		return false;
	}
}


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

bool MysqlDao::GetThreadType(long long thread_id, int& thread_type)
{
	thread_type = static_cast<int>(ThreadType::Private);
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		auto row = con->sql("SELECT thread_type FROM conversation WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		if (row.isNull())
			return false;
		thread_type = row[0].get<int>();
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading thread type: " << e.what();
		return false;
	}
}

int MysqlDao::GetThreadTypeCached(long long thread_id)
{
	{
		std::lock_guard<std::mutex> lock(_type_mutex);
		auto iter = _type_cache.find(thread_id);
		if (iter != _type_cache.end())
			return iter->second;
	}

	//未命中才查库；查询失败或会话不存在时按单聊继续，
	//至少不会把一条本可送达的私聊消息吞掉
	int type = static_cast<int>(ThreadType::Private);
	if (!GetThreadType(thread_id, type))
		return static_cast<int>(ThreadType::Private);

	{
		std::lock_guard<std::mutex> lock(_type_mutex);
		//同一 thread 的并发未命中可能重复插入，只有第一次插入才记入淘汰队列
		if (_type_cache.emplace(thread_id, type).second) {
			_type_order.push_back(thread_id);
			if (_type_cache.size() > kThreadTypeCacheMax) {
				_type_cache.erase(_type_order.front());
				_type_order.pop_front();
			}
		}
	}
	return type;
}
