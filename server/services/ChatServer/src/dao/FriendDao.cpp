#include "MysqlDao.h"
#include "JsonUtil.h"
#include <algorithm>
#include <json/json.h>
#include "Log.h"

namespace {
//标签以 JSON 数组存一列: 标签里可能带逗号或引号, 用标准转义避免自定义分隔符的歧义
std::string TagsToJson(const std::vector<std::string>& tags)
{
	Json::Value array(Json::arrayValue);
	for (const auto& tag : tags)
		array.append(tag);
	Json::StreamWriterBuilder builder;
	builder["indentation"] = ""; //紧凑输出, 这一列只存数据不给人看
	//中文按 UTF-8 原样写, 默认的 \uXXXX 转义会把一个汉字撑到 6 字节, 接近列宽上限
	builder["emitUTF8"] = true;
	return Json::writeString(builder, array);
}

//解析 tags 列; 空串、非法 JSON 或非数组都按"没有标签"处理, 不让脏数据影响好友列表
std::vector<std::string> JsonToTags(const std::string& text)
{
	std::vector<std::string> tags;
	if (text.empty())
		return tags;

	Json::Value array;
	if (!JsonFromString(text, array) || !array.isArray())
		return tags;

	for (const auto& item : array) {
		if (!item.isString())
			continue;
		const auto tag = item.asString();
		if (!tag.empty())
			tags.push_back(tag);
	}
	return tags;
}
} // namespace

bool MysqlDao::AddFriendApply(const int& from, const int& to, const std::string& desc,
	const std::string& back_name, const std::vector<std::string>& tags)
{
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
	if (!con)
		return false;

	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏

	try {
		//申请留言、备注和分组标签都保存在申请记录里，同意时一次性复制到好友关系行；
		//重新申请时把状态复位成 0（未处理），否则删过好友的人申请后对方看不到这条记录
		con->sql(
			"INSERT INTO friend_apply (from_uid, to_uid, descs, back_name, tags) "
			"VALUES (?, ?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE descs = VALUES(descs), back_name = VALUES(back_name), "
			"tags = VALUES(tags), status = 0")
			.bind(from).bind(to).bind(desc).bind(back_name).bind(TagsToJson(tags)).execute();
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

bool MysqlDao::AddFriend(const int& from, const int& to, std::string back_name,
	const std::vector<std::string>& tags, std::string& apply_desc,
	std::string& reverse_back, std::vector<std::string>& reverse_tags,
	bool& first_approve) {
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//开始事务
		con->startTransaction();

        //from是当前同意申请的用户，to是原申请人，所以查找申请时要按to到from的方向匹配
        //FOR UPDATE在事务内锁住这条申请，后面的状态修改和双向好友写入与它一起提交或回滚
        auto apply = con->sql(
            "SELECT status, descs, back_name, tags FROM friend_apply "
            "WHERE from_uid = ? AND to_uid = ? FOR UPDATE")
            .bind(to).bind(from).execute().fetchOne();
        if (apply.isNull()) { //没有对应申请时不允许直接建立好友关系
            con->rollback();
            return false;
        }
        //状态在 0 时才写欢迎语与推送；重复点同意只回包，不再制造重复消息
        first_approve = apply[0].get<int>() == 0;
        apply_desc = apply[1].get<std::string>();
        //reverse_back / reverse_tags 是申请方当初给通过方设的备注与分组，回传给申请方用于本地显示
        reverse_back = apply[2].isNull() ? std::string() : apply[2].get<std::string>();
        reverse_tags = JsonToTags(apply[3].isNull() ? std::string() : apply[3].get<std::string>());
        //申请已确认时再次设为1不会产生额外状态变化，后续INSERT IGNORE也可避免重复插入好友关系
        con->sql("UPDATE friend_apply SET status = 1 WHERE from_uid = ? AND to_uid = ?")
            .bind(to).bind(from).execute();

		//两条好友关系必须按 uid 从小到大写入: 对方同时同意我的申请时, 两个事务插入的是
		//同一对行, 只是自身视角不同; 按固定顺序加锁才能避免 AB/BA 循环等待造成的死锁
		const bool from_is_lower = from < to;
		const int lower_self = from_is_lower ? from : to;
		const int lower_friend = from_is_lower ? to : from;
		const std::string& lower_back = from_is_lower ? back_name : reverse_back;
		const int higher_self = from_is_lower ? to : from;
		const int higher_friend = from_is_lower ? from : to;
		const std::string& higher_back = from_is_lower ? reverse_back : back_name;

		//标签跟随各自方向: 同意方填的归同意方这行, 申请时填的归申请方那行
		const std::string my_tags = TagsToJson(tags);
		const std::string peer_tags = TagsToJson(reverse_tags);
		const std::string& lower_tags = from_is_lower ? my_tags : peer_tags;
		const std::string& higher_tags = from_is_lower ? peer_tags : my_tags;

		//第一条: uid 较小的一方作为 self_id。
		//重新同意申请时不能用 INSERT IGNORE（新备注会被静默丢弃），也不能无条件覆盖：
		//本次没填备注/标签时保留原值，避免一次空提交把用户之前设置的数据清掉
		con->sql("INSERT INTO friend(self_id, friend_id, `back`, tags) "
			"VALUES (?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE "
			"`back` = IF(VALUES(`back`) = '', `back`, VALUES(`back`)), "
			"tags = IF(VALUES(tags) = '[]', tags, VALUES(tags))")
			.bind(lower_self).bind(lower_friend).bind(lower_back).bind(lower_tags).execute();

		//第二条: uid 较大的一方作为 self_id, 备注与标签跟随各自的方向
		con->sql("INSERT INTO friend(self_id, friend_id, `back`, tags) "
			"VALUES (?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE "
			"`back` = IF(VALUES(`back`) = '', `back`, VALUES(`back`)), "
			"tags = IF(VALUES(tags) = '[]', tags, VALUES(tags))")
			.bind(higher_self).bind(higher_friend).bind(higher_back).bind(higher_tags).execute();

		//提交事务
		con->commit();
		return true;
	}
	catch (MysqlError& e) {
		//如果发生错误, 回滚事务; 连接已断时回滚本身会抛异常, 不能让它逃出本函数
		if (con) {
			try {
				con->rollback();
			}
			catch (MysqlError& rollback_error) {
				LOG_ERROR << "rollback failed, error is " << rollback_error.what();
			}
		}
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

//修改自己给好友设的备注与标签: 返回 1 已更新, 0 不是好友, -1 数据库错误
//备注和标签在同一行, 一条 UPDATE 就能整组替换, 不会出现只改成功一半
int MysqlDao::UpdateFriendRemark(int self_id, int friend_id, const std::string& remark,
	const std::vector<std::string>& tags)
{
	auto con = pool_->Acquire();
	if (!con)
		return -1;


	try {
		//备注没变化时 UPDATE 的影响行数为 0, 不能据此判断关系是否存在, 所以先单独查一次
		auto row = con->sql("SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(self_id).bind(friend_id).execute().fetchOne();
		if (row.isNull())
			return 0;

		//这里只改 self_id 自己这一行, 对方看到的仍然是他给我设的备注与标签
		con->sql("UPDATE friend SET `back` = ?, tags = ? "
			"WHERE self_id = ? AND friend_id = ?")
			.bind(remark).bind(TagsToJson(tags)).bind(self_id).bind(friend_id).execute();
		return 1;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return -1;
	}
}

bool MysqlDao::GetApplyList(int touid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	auto con = pool_->Acquire(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
	if (!con)
		return false;

	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏

	try {
		//申请列表直接关联用户表取当前资料，头像必须一起查，
		//否则申请人换了头像后「新的朋友」仍只能显示空图标
		auto res = con->sql("select apply.from_uid, apply.status, user.name, user.nick, user.sex, user.icon, apply.descs, apply.id "
			"from friend_apply as apply join user on apply.from_uid = user.uid where apply.to_uid = ? "
			"and apply.id > ? order by apply.id DESC LIMIT ? ")
			.bind(touid).bind(begin).bind(limit).execute();

		auto row = res.fetchOne();
		//遍历结果集
		while (!row.isNull()) {
			auto name = row[2].get<std::string>();
			auto uid = row[0].get<int>();
			auto status = row[1].get<int>();
			auto nick = row[3].get<std::string>();
			auto sex = row[4].get<int>();
			auto icon = row[5].get<std::string>();
			auto desc = row[6].get<std::string>();
			//将查询结果中的好友申请信息封装成ApplyInfo对象
			auto apply_ptr = std::make_shared<ApplyInfo>(uid, name, desc, icon, nick, sex, status);
			apply_ptr->_id = row[7].get<int64_t>();
			applyList.push_back(apply_ptr); //将查询结果中的好友申请信息存储在applyList中
			row = res.fetchOne();
		}

		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

//我发出的申请: 对方是被申请人, 所以按 to_uid 关联用户资料
bool MysqlDao::GetSentApplyList(int from_uid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//我发出的申请展示的是对方头像，同样要取 user.icon
		auto res = con->sql(
			"select apply.to_uid, apply.status, user.name, user.nick, user.sex, user.icon, apply.descs, apply.id "
			"from friend_apply as apply join user on apply.to_uid = user.uid where apply.from_uid = ? "
			"and apply.id > ? order by apply.id DESC LIMIT ? ")
			.bind(from_uid).bind(begin).bind(limit).execute();

		auto row = res.fetchOne();
		while (!row.isNull()) {
			auto uid = row[0].get<int>();
			auto status = row[1].get<int>();
			auto name = row[2].get<std::string>();
			auto nick = row[3].get<std::string>();
			auto sex = row[4].get<int>();
			auto icon = row[5].get<std::string>();
			auto desc = row[6].get<std::string>();
			//desc 存的是我填的留言, 界面据此显示自己发出去的内容
			auto apply_ptr = std::make_shared<ApplyInfo>(uid, name, desc, icon, nick, sex, status);
			apply_ptr->_id = row[7].get<int64_t>();
			applyList.push_back(apply_ptr);
			row = res.fetchOne();
		}

		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading sent apply list: " << e.what();
		return false;
	}
}

//关系判定统一走这里：搜索页据此决定给"添加/已申请/发消息"，客户端不再靠本地缓存猜。
//单向行说明被删过，既不是好友也不能显示"已申请"，要允许重新申请
int MysqlDao::GetFriendRelation(int me, int peer)
{
	if (me <= 0 || peer <= 0 || me == peer)
		return 0;
	auto con = pool_->Acquire();
	if (!con)
		return 0;
	try {
		auto row = con->sql(
			"SELECT "
			"(SELECT COUNT(*) FROM friend WHERE self_id = ? AND friend_id = ?), "
			"(SELECT COUNT(*) FROM friend WHERE self_id = ? AND friend_id = ?), "
			"(SELECT COUNT(*) FROM friend_apply WHERE from_uid = ? AND to_uid = ? AND status = 0), "
			"(SELECT COUNT(*) FROM friend_apply WHERE from_uid = ? AND to_uid = ? AND status = 0)")
			.bind(me).bind(peer).bind(peer).bind(me)
			.bind(me).bind(peer).bind(peer).bind(me).execute().fetchOne();
		if (row.isNull())
			return 0;
		const int mine = row[0].get<int>();
		const int theirs = row[1].get<int>();
		if (mine > 0 && theirs > 0)
			return 1;
		if (mine > 0 || theirs > 0)
			return 4; //单向关系：之前是好友，其中一方删除了
		if (row[2].get<int>() > 0)
			return 2;
		if (row[3].get<int>() > 0)
			return 3;
		return 0;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error loading friend relation: " << e.what();
		return 0;
	}
}

//删除好友：只删自己这一行，对方那行保留——他仍然把你看作好友，也还能看到会话。
//两边都删了，关系才算彻底消失；会话与消息始终保留
bool MysqlDao::DeleteFriend(int uid, int touid)
{
	if (uid <= 0 || touid <= 0 || uid == touid)
		return false;
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		con->startTransaction();
		con->sql("DELETE FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(uid).bind(touid).execute();
		//只清自己发出的申请：对方发起的记录属于他那侧的「新的朋友」，不该被我删掉
		con->sql("DELETE FROM friend_apply WHERE from_uid = ? AND to_uid = ?")
			.bind(uid).bind(touid).execute();

		//双向关系都断了也不删消息：历史属于双方，留给保留策略（归档）处理。
		//会话与成员行同样保留，thread_id 不变，重新加好友后历史与游标都能接上
		con->commit();
		return true;
	}
	catch (MysqlError& e) {
		try { con->rollback(); }
		catch (...) {}
		LOG_ERROR << "mysql error deleting friend: " << e.what();
		return false;
	}
}

//双向关系是否完整：只有两行都在，消息才发得出去。
//任何一方删了好友，另一方还能看到会话，但发送会被这里挡住
bool MysqlDao::IsMutualFriend(int uid, int touid)
{
	if (uid <= 0 || touid <= 0 || uid == touid)
		return false;
	auto con = pool_->Acquire();
	if (!con)
		return false;
	try {
		auto row = con->sql(
			"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
			"OR (self_id = ? AND friend_id = ?)")
			.bind(uid).bind(touid).bind(touid).bind(uid).execute().fetchOne();
		return !row.isNull() && row[0].get<int>() == 2;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error checking mutual friend: " << e.what();
		return false;
	}
}

//取好友 uid 列表: 资料变更推送只需要 id, 不重复查用户资料
bool MysqlDao::GetFriendIds(int uid, std::vector<int>& friend_ids)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		auto res = con->sql("SELECT friend_id FROM friend WHERE self_id = ?")
			.bind(uid).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			friend_ids.push_back(row[0].get<int>());
			row = res.fetchOne();
		}
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}

//把该会话里"发给 uid"的消息标为已读。只更新 recv_id = uid 的行，

bool MysqlDao::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info_list)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//备注、标签和用户资料一次 JOIN 取回：原实现每个好友再查一次 user 表，
		//500 个好友就是 501 次查询，登录高峰会把单逻辑线程和 5 条连接全部占满
		auto res = con->sql(
			"SELECT f.friend_id, f.`back`, f.tags, u.name, u.nick, u.icon, u.sex, u.`desc` "
			"FROM friend f JOIN user u ON u.uid = f.friend_id "
			"WHERE f.self_id = ? ORDER BY f.friend_id")
			.bind(self_id).execute();

		auto row = res.fetchOne();
		//遍历结果集
		while (!row.isNull()) {
			auto user_info = std::make_shared<UserInfo>();
			user_info->uid = row[0].get<int>();
			//备注是本人给好友起的名字, 允许为空; 空串表示没设, 由客户端回落到昵称或用户名
			user_info->back = row[1].isNull() ? std::string() : row[1].get<std::string>();
			user_info->tags = JsonToTags(row[2].isNull() ? std::string() : row[2].get<std::string>());
			user_info->name = row[3].get<std::string>();
			user_info->nick = row[4].get<std::string>();
			user_info->icon = row[5].get<std::string>();
			user_info->sex = row[6].get<int>();
			user_info->desc = row[7].get<std::string>();
			user_info_list.push_back(user_info);
			row = res.fetchOne();
		}
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error: " << e.what();
		return false;
	}
}
