#include "MysqlDao.h"
#include "ConfigMgr.h"
#include <cstdlib>
#include <cstdint>
#include <algorithm>
#include <json/json.h>
#include <json/writer.h>

MysqlDao::MysqlDao()
{
	auto& cfg = ConfigMgr::Inst(); //获取配置管理器实例
	const auto& host = cfg["Mysql"]["Host"];
	const auto& xport = cfg["Mysql"]["XPort"]; //X Protocol端口号
	const auto& pwd = cfg["Mysql"]["Passwd"];
	const auto& schema = cfg["Mysql"]["Schema"];
	const auto& user = cfg["Mysql"]["User"];
	//X Protocol默认端口为33060, 若配置缺失则使用默认值
	unsigned xport_num = 33060;
	if (!xport.empty()) {
		int v = std::atoi(xport.c_str());
		if (v > 0) {
			xport_num = static_cast<unsigned>(v);
		}
	}
	//初始化MySQL连接池对象, 连接池大小为5
	pool_.reset(new MySqlPool(host, xport_num, user, pwd, schema, 5));
	//标签存在关系表的列里, 首次启动补齐; 失败只记日志, 不影响登录与聊天
	EnsureSchema();
}

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
	Json::Reader reader;
	if (!reader.parse(text, array) || !array.isArray())
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
}

//启动补列：先查 information_schema，缺列才 ALTER，重复启动不会报错。
//好友标签、消息的已读/撤回标记与群聊设置都属于"后加字段"，统一在这里补齐
bool MysqlDao::EnsureSchema()
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//表名与列定义都是固定字面量, 不来自外部输入
		auto has_column = [&con](const std::string& table, const std::string& column) {
			auto res = con->_session->sql(
				"SELECT COUNT(*) FROM information_schema.columns "
				"WHERE table_schema = DATABASE() AND table_name = ? AND column_name = ?")
				.bind(table).bind(column).execute().fetchOne();
			return !res.isNull() && res[0].get<int>() > 0;
		};
		auto has_table = [&con](const std::string& table) {
			auto res = con->_session->sql(
				"SELECT COUNT(*) FROM information_schema.tables "
				"WHERE table_schema = DATABASE() AND table_name = ?")
				.bind(table).execute().fetchOne();
			return !res.isNull() && res[0].get<int>() > 0;
		};

		//512 字节够 10 个 12 字的中文标签再加 JSON 符号
		for (const char* table : {"friend", "friend_apply"}) {
			if (!has_column(table, "tags")) {
				con->_session->sql("ALTER TABLE " + std::string(table) +
					" ADD COLUMN tags VARCHAR(512) NOT NULL DEFAULT '[]'").execute();
			}
		}

		//已读只对接收方有意义；撤回是双方可见的状态，正文会被清空
		if (!has_column("chat_message", "is_read")) {
			con->_session->sql("ALTER TABLE chat_message "
				"ADD COLUMN is_read TINYINT UNSIGNED NOT NULL DEFAULT 0").execute();
		}
		if (!has_column("chat_message", "recalled")) {
			con->_session->sql("ALTER TABLE chat_message "
				"ADD COLUMN recalled TINYINT UNSIGNED NOT NULL DEFAULT 0").execute();
		}
		//群消息的 @ 名单：逗号分隔的 uid，未读查询用 FIND_IN_SET 判断"有没有 @我"
		if (!has_column("chat_message", "mentions")) {
			con->_session->sql("ALTER TABLE chat_message "
				"ADD COLUMN mentions VARCHAR(255) NOT NULL DEFAULT ''").execute();
		}

		//群聊表在课程设计里已经存在，这里兜住新建环境：缺表时按同一套结构补建
		if (!has_table("group_chat")) {
			con->_session->sql(
				"CREATE TABLE IF NOT EXISTS group_chat ("
				"thread_id BIGINT UNSIGNED NOT NULL,"
				"group_name VARCHAR(255) NOT NULL DEFAULT '',"
				"owner_id INT NOT NULL DEFAULT 0,"
				"notice VARCHAR(500) NOT NULL DEFAULT '',"
				"created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
				"updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,"
				"PRIMARY KEY (thread_id),"
				"CONSTRAINT fk_group_thread FOREIGN KEY (thread_id) "
				"REFERENCES chat_thread (thread_id) ON DELETE CASCADE"
				") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci").execute();
		}
		if (!has_table("group_chat_member")) {
			con->_session->sql(
				"CREATE TABLE IF NOT EXISTS group_chat_member ("
				"thread_id BIGINT UNSIGNED NOT NULL,"
				"user_id INT NOT NULL,"
				"role TINYINT UNSIGNED NOT NULL DEFAULT 0,"
				"nickname VARCHAR(32) NOT NULL DEFAULT '',"
				"last_read_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0,"
				"muted TINYINT UNSIGNED NOT NULL DEFAULT 0,"
				"joined_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,"
				"PRIMARY KEY (thread_id, user_id),"
				"KEY idx_group_member_user (user_id, thread_id),"
				"CONSTRAINT fk_group_member_thread FOREIGN KEY (thread_id) "
				"REFERENCES group_chat (thread_id) ON DELETE CASCADE"
				") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci").execute();
		}

		//群名与公告要能改，更新时间用于后续排查；成员表补群昵称、已读游标与免打扰
		if (!has_column("group_chat", "notice")) {
			con->_session->sql("ALTER TABLE group_chat "
				"ADD COLUMN notice VARCHAR(500) NOT NULL DEFAULT ''").execute();
		}
		if (!has_column("group_chat", "updated_at")) {
			con->_session->sql("ALTER TABLE group_chat ADD COLUMN updated_at TIMESTAMP "
				"NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP").execute();
		}
		if (!has_column("group_chat_member", "nickname")) {
			con->_session->sql("ALTER TABLE group_chat_member "
				"ADD COLUMN nickname VARCHAR(32) NOT NULL DEFAULT ''").execute();
		}
		if (!has_column("group_chat_member", "last_read_message_id")) {
			con->_session->sql("ALTER TABLE group_chat_member ADD COLUMN "
				"last_read_message_id BIGINT UNSIGNED NOT NULL DEFAULT 0").execute();
		}
		if (!has_column("group_chat_member", "muted")) {
			con->_session->sql("ALTER TABLE group_chat_member "
				"ADD COLUMN muted TINYINT UNSIGNED NOT NULL DEFAULT 0").execute();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "ensure schema failed: " << e.what() << std::endl;
		return false;
	}
}

MysqlDao::~MysqlDao() {
	pool_->Close(); //关闭连接池, 释放资源
}

int MysqlDao::RegUser(const std::string& name, const std::string& email, const std::string& pwd)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回-1
		if (con == nullptr)
			return -1;

		//调用存储过程, 通过会话变量@result返回结果(与JDBC API时代逻辑一致)
		con->_session->sql("CALL reg_user(?,?,?,@result)")
			.bind(name).bind(email).bind(pwd).execute();

		//查询会话变量获取输出参数的值(X DevAPI不直接支持注册输出参数)
		auto res = con->_session->sql("SELECT @result AS result").execute();

		//解析结果, 如果查询成功且有结果, 则获取result值并返回, 否则返回-1表示失败
		auto row = res.fetchOne();
		if (!row.isNull()) {
			int result = row[0].get<int>();
			std::cout << "Result: " << result << std::endl;
			pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
			return result;
		}
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		return -1;
	}catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return -1;
	}
}

bool MysqlDao::CheckEmail(const std::string& name, const std::string& email) {
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (con == nullptr)
			return false;

		//执行查询语句(查询指定用户名的邮箱地址)
		auto res = con->_session->sql("SELECT email FROM user WHERE name = ?")
			.bind(name).execute();

		auto row = res.fetchOne();
		//遍历结果集
		while (!row.isNull()) {
			auto email_in_db = row[0].get<std::string>();
			std::cout << "Check Email: " << email_in_db << std::endl;
			//如果查询结果中的email与输入的email不匹配, 则说明用户名和邮箱不匹配, 返回false
			if (email != email_in_db) {
				pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
				return false;
			}
			pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
			return true;
			row = res.fetchOne();
		}
		//保持原行为: 用户名不存在时同样返回true
		pool_->returnConnection(std::move(con));
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::UpdatePwd(const std::string& name, const std::string& newpwd) {
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (con == nullptr)
			return false;

		//执行更新语句(更新指定用户名的密码)
		auto res = con->_session->sql("UPDATE user SET pwd = ? WHERE name = ?")
			.bind(newpwd).bind(name).execute();

		std::cout << "Updated rows: " << res.getAffectedItemsCount() << std::endl;
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::CheckPwd(const std::string& email, const std::string& pwd, UserInfo& userInfo) {
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con)); //将使用完的会话对象归还给连接池
		});

	try {
		//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
		if (con == nullptr)
			return false;

		//执行查询语句(查询指定邮箱的用户信息, 显式列出列以便按索引取值)
		auto res = con->_session->sql("SELECT uid, name, pwd, email FROM user WHERE email = ?")
			.bind(email).execute();

		std::string origin_pwd = "";
		auto row = res.fetchOne();
		//遍历结果集
		if (!row.isNull()) {
			origin_pwd = row[2].get<std::string>(); //第2列为pwd
		}

		//如果查询结果中的密码与输入的密码不匹配, 则说明邮箱和密码不匹配, 返回false
		if (pwd != origin_pwd)
			return false;

		//如果邮箱和密码匹配, 则将查询结果中的用户信息存储在userInfo中, 返回true
		userInfo.name = row[1].get<std::string>();
		userInfo.email = email;
		userInfo.uid = row[0].get<int>();
		userInfo.pwd = origin_pwd;
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::AddFriendApply(const int& from, const int& to, const std::string& desc,
	const std::string& back_name, const std::vector<std::string>& tags)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
	if (con == nullptr)
		return false;

	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//申请留言、备注和分组标签都保存在申请记录里，同意时一次性复制到好友关系行；
		//重新申请时把状态复位成 0（未处理），否则删过好友的人申请后对方看不到这条记录
		con->_session->sql(
			"INSERT INTO friend_apply (from_uid, to_uid, descs, back_name, tags) "
			"VALUES (?, ?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE descs = VALUES(descs), back_name = VALUES(back_name), "
			"tags = VALUES(tags), status = 0")
			.bind(from).bind(to).bind(desc).bind(back_name).bind(TagsToJson(tags)).execute();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::AddFriend(const int& from, const int& to, std::string back_name,
	const std::vector<std::string>& tags, std::string& apply_desc,
	std::string& reverse_back, std::vector<std::string>& reverse_tags,
	bool& first_approve) {
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//开始事务
		con->_session->startTransaction();

        //from是当前同意申请的用户，to是原申请人，所以查找申请时要按to到from的方向匹配
        //FOR UPDATE在事务内锁住这条申请，后面的状态修改和双向好友写入与它一起提交或回滚
        auto apply = con->_session->sql(
            "SELECT status, descs, back_name, tags FROM friend_apply "
            "WHERE from_uid = ? AND to_uid = ? FOR UPDATE")
            .bind(to).bind(from).execute().fetchOne();
        if (apply.isNull()) { //没有对应申请时不允许直接建立好友关系
            con->_session->rollback();
            return false;
        }
        //状态在 0 时才写欢迎语与推送；重复点同意只回包，不再制造重复消息
        first_approve = apply[0].get<int>() == 0;
        apply_desc = apply[1].get<std::string>();
        //reverse_back / reverse_tags 是申请方当初给通过方设的备注与分组，回传给申请方用于本地显示
        reverse_back = apply[2].isNull() ? std::string() : apply[2].get<std::string>();
        reverse_tags = JsonToTags(apply[3].isNull() ? std::string() : apply[3].get<std::string>());
        //申请已确认时再次设为1不会产生额外状态变化，后续INSERT IGNORE也可避免重复插入好友关系
        con->_session->sql("UPDATE friend_apply SET status = 1 WHERE from_uid = ? AND to_uid = ?")
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

		//第一条: uid 较小的一方作为 self_id
		con->_session->sql("INSERT IGNORE INTO friend(self_id, friend_id, `back`, tags) "
			"VALUES (?, ?, ?, ?) ")
			.bind(lower_self).bind(lower_friend).bind(lower_back).bind(lower_tags).execute();

		//第二条: uid 较大的一方作为 self_id, 备注与标签跟随各自的方向
		con->_session->sql("INSERT IGNORE INTO friend(self_id, friend_id, `back`, tags) "
			"VALUES (?, ?, ?, ?) ")
			.bind(higher_self).bind(higher_friend).bind(higher_back).bind(higher_tags).execute();

		//提交事务
		con->_session->commit();
		std::cout << "addfriend insert friends success" << std::endl;

		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		//如果发生错误, 回滚事务; 连接已断时回滚本身会抛异常, 不能让它逃出本函数
		if (con) {
			try {
				con->_session->rollback();
			}
			catch (mysqlx::Error& rollback_error) {
				std::cerr << "rollback failed, error is " << rollback_error.what() << std::endl;
			}
		}
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

//更新本人可编辑的资料字段; 昵称与签名的长度由调用方按字符数校验
bool MysqlDao::UpdateUserProfile(int uid, const std::string& nick, int sex, const std::string& desc)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//desc 是MySQL保留字, 必须用反引号包起来; 三个字段一起写, 空串表示清空
		con->_session->sql("UPDATE user SET nick = ?, sex = ?, `desc` = ? WHERE uid = ?")
			.bind(nick).bind(sex).bind(desc).bind(uid).execute();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

//修改自己给好友设的备注与标签: 返回 1 已更新, 0 不是好友, -1 数据库错误
//备注和标签在同一行, 一条 UPDATE 就能整组替换, 不会出现只改成功一半
int MysqlDao::UpdateFriendRemark(int self_id, int friend_id, const std::string& remark,
	const std::vector<std::string>& tags)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return -1;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//备注没变化时 UPDATE 的影响行数为 0, 不能据此判断关系是否存在, 所以先单独查一次
		auto row = con->_session->sql("SELECT 1 FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(self_id).bind(friend_id).execute().fetchOne();
		if (row.isNull())
			return 0;

		//这里只改 self_id 自己这一行, 对方看到的仍然是他给我设的备注与标签
		con->_session->sql("UPDATE friend SET `back` = ?, tags = ? "
			"WHERE self_id = ? AND friend_id = ?")
			.bind(remark).bind(TagsToJson(tags)).bind(self_id).bind(friend_id).execute();
		return 1;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return -1;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回空指针
	if (con == nullptr) {
		return nullptr;
	}
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//执行查询语句(查询指定用户ID的用户信息, 显式列出列以便按索引取值)
		auto res = con->_session->sql("SELECT uid, name, pwd, email, nick, `desc`, sex, icon "
			"FROM user WHERE uid = ?")
			.bind(uid).execute();

		std::shared_ptr<UserInfo> user_ptr = nullptr;
		auto row = res.fetchOne();
		//遍历结果集
		if (!row.isNull()) {
			//如果查询结果中有数据, 则说明用户ID存在, 将查询结果中的用户信息存储在user_ptr中, 返回user_ptr
			user_ptr.reset(new UserInfo);
			user_ptr->uid = row[0].get<int>();
			user_ptr->email = row[3].get<std::string>();
			user_ptr->name = row[1].get<std::string>();
			user_ptr->pwd = row[2].get<std::string>();
			user_ptr->nick = row[4].get<std::string>();
			user_ptr->desc = row[5].get<std::string>();
			user_ptr->sex = row[6].get<int>();
			user_ptr->icon = row[7].get<std::string>();
		}
		return user_ptr;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return nullptr;
	}
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(const std::string& name)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回空指针
	if (con == nullptr) {
		return nullptr;
	}
	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//执行查询语句(查询指定用户名的用户信息, 与JDBC API时代一致, 不读取icon字段)
		auto res = con->_session->sql("SELECT uid, name, pwd, email, nick, `desc`, sex "
			"FROM user WHERE name = ?")
			.bind(name).execute();

		std::shared_ptr<UserInfo> user_ptr = nullptr;
		auto row = res.fetchOne();
		//遍历结果集
		if (!row.isNull()) {
			//如果查询结果中有数据, 则说明用户名存在, 将查询结果中的用户信息存储在user_ptr中, 返回user_ptr
			user_ptr.reset(new UserInfo);
			user_ptr->uid = row[0].get<int>();
			user_ptr->email = row[3].get<std::string>();
			user_ptr->name = name;
			user_ptr->pwd = row[2].get<std::string>();
			user_ptr->nick = row[4].get<std::string>();
			user_ptr->desc = row[5].get<std::string>();
			user_ptr->sex = row[6].get<int>();
		}
		return user_ptr;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return nullptr;
	}
}

bool MysqlDao::GetApplyList(int touid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	auto con = pool_->getConnection(); //从连接池中获取一个可用的MySQL会话对象
	//如果连接池返回空指针, 则说明连接池已停止, 直接返回false
	if (con == nullptr)
		return false;

	//使用RAII机制确保无论函数如何退出, 都能正确归还MySQL会话对象给连接池, 避免资源泄漏
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//申请列表直接关联用户表取当前资料，头像必须一起查，
		//否则申请人换了头像后「新的朋友」仍只能显示空图标
		auto res = con->_session->sql("select apply.from_uid, apply.status, user.name, user.nick, user.sex, user.icon, apply.descs, apply.id "
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
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

//我发出的申请: 对方是被申请人, 所以按 to_uid 关联用户资料
bool MysqlDao::GetSentApplyList(int from_uid,
	std::vector<std::shared_ptr<ApplyInfo>>& applyList, int begin, int limit)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//我发出的申请展示的是对方头像，同样要取 user.icon
		auto res = con->_session->sql(
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
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error loading sent apply list: " << e.what() << std::endl;
		return false;
	}
}

//关系判定统一走这里：搜索页据此决定给"添加/已申请/发消息"，客户端不再靠本地缓存猜。
//单向行说明被删过，既不是好友也不能显示"已申请"，要允许重新申请
int MysqlDao::GetFriendRelation(int me, int peer)
{
	if (me <= 0 || peer <= 0 || me == peer)
		return 0;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return 0;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto row = con->_session->sql(
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
	catch (mysqlx::Error& e) {
		con->_healthy = false;
		std::cerr << "Mysqlx Error loading friend relation: " << e.what() << std::endl;
		return 0;
	}
}

//删好友前确认关系还在：删过的会话不该再能发消息
bool MysqlDao::IsFriend(int uid, int touid)
{
	if (uid <= 0 || touid <= 0 || uid == touid)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	try {
		auto row = con->_session->sql(
			"SELECT id FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(uid).bind(touid).execute().fetchOne();
		return !row.isNull();
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error checking friend: " << e.what() << std::endl;
		return false;
	}
}

//删除好友：只删自己这一行，对方那行保留——他仍然把你看作好友，也还能看到会话。
//两边都删了，关系才算彻底消失；会话与消息始终保留
bool MysqlDao::DeleteFriend(int uid, int touid)
{
	if (uid <= 0 || touid <= 0 || uid == touid)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	try {
		con->_session->startTransaction();
		con->_session->sql("DELETE FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(uid).bind(touid).execute();
		//只清自己发出的申请：对方发起的记录属于他那侧的「新的朋友」，不该被我删掉
		con->_session->sql("DELETE FROM friend_apply WHERE from_uid = ? AND to_uid = ?")
			.bind(uid).bind(touid).execute();
		con->_session->commit();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error deleting friend: " << e.what() << std::endl;
		return false;
	}
}

//双向关系是否完整：只有两行都在，消息才发得出去。
//任何一方删了好友，另一方还能看到会话，但发送会被这里挡住
bool MysqlDao::IsMutualFriend(int uid, int touid)
{
	if (uid <= 0 || touid <= 0 || uid == touid)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});
	try {
		auto row = con->_session->sql(
			"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
			"OR (self_id = ? AND friend_id = ?)")
			.bind(uid).bind(touid).bind(touid).bind(uid).execute().fetchOne();
		return !row.isNull() && row[0].get<int>() == 2;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error checking mutual friend: " << e.what() << std::endl;
		return false;
	}
}

//取好友 uid 列表: 资料变更推送只需要 id, 不重复查用户资料
bool MysqlDao::GetFriendIds(int uid, std::vector<int>& friend_ids)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		auto res = con->_session->sql("SELECT friend_id FROM friend WHERE self_id = ?")
			.bind(uid).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			friend_ids.push_back(row[0].get<int>());
			row = res.fetchOne();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

//把该会话里"发给 uid"的消息标为已读。只更新 recv_id = uid 的行，
//发送方自己那一侧的同一条记录不受影响——已读状态永远是接收方视角
bool MysqlDao::MarkChatMessagesRead(int uid, long long thread_id, long long last_message_id,
	bool& changed, int& peer_uid)
{
	changed = false;
	peer_uid = 0;
	if (uid <= 0 || thread_id <= 0 || last_message_id <= 0)
		return false;

	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//先确认请求者在这个会话里，不能拿别人的 thread_id 改已读；顺手取对方 uid 用于通知
		auto member = con->_session->sql(
			"SELECT CASE WHEN user1_id = ? THEN user2_id ELSE user1_id END "
			"FROM private_chat WHERE thread_id = ? AND (user1_id = ? OR user2_id = ?)")
			.bind(uid).bind(thread_id).bind(uid).bind(uid).execute().fetchOne();
		if (member.isNull())
			return false;
		peer_uid = member[0].get<int>();

		auto result = con->_session->sql(
			"UPDATE chat_message SET is_read = 1 "
			"WHERE thread_id = ? AND recv_id = ? AND message_id <= ? AND is_read = 0")
			.bind(thread_id).bind(uid).bind(static_cast<int64_t>(last_message_id)).execute();
		changed = result.getAffectedItemsCount() > 0;
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error marking read: " << e.what() << std::endl;
		return false;
	}
}

//撤回自己发的消息：只允许发送者本人、2 分钟内、未撤回。
//撤回不物理删除，改为 recalled=1 并清空正文，双方拉历史时都只能看到提示
int MysqlDao::RecallChatMessage(int uid, long long message_id, ChatMessageInfo& message)
{
	if (uid <= 0 || message_id <= 0)
		return ErrorCodes::RecallDenied;

	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		auto row = con->_session->sql(
			"SELECT thread_id, sender_id, recv_id, client_msg_id, UNIX_TIMESTAMP(created_at), "
			"is_read, recalled "
			"FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		if (row.isNull() || row[1].get<int>() != uid)
			return ErrorCodes::RecallDenied;
		if (row[6].get<int>() != 0)
			return ErrorCodes::RecallDenied;

		const long long created_at = row[4].get<int64_t>();
		if (std::time(nullptr) - created_at > RECALL_WINDOW_SECONDS)
			return ErrorCodes::RecallExpired;

		con->_session->sql(
			"UPDATE chat_message SET recalled = 1, content = '', msg_type = 0 "
			"WHERE message_id = ? AND recalled = 0")
			.bind(static_cast<int64_t>(message_id)).execute();

		message.message_id = message_id;
		message.thread_id = row[0].get<int64_t>();
		message.sender_id = uid;
		message.recv_id = row[2].get<int>();
		message.client_msg_id = row[3].get<std::string>();
		message.content.clear();
		message.msg_type = 0;
		message.created_at = created_at;
		message.is_read = row[5].get<int>() != 0;
		message.recalled = true;
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error recalling message: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

bool MysqlDao::GetFriendList(int self_id, std::vector<std::shared_ptr<UserInfo> >& user_info_list)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//备注与标签都在关系行里, 一次查回来, 不再逐好友查第二张表
		auto res = con->_session->sql("select friend_id, `back`, tags from friend where self_id = ? ")
			.bind(self_id).execute();

		auto row = res.fetchOne();
		//遍历结果集
		while (!row.isNull()) {
			auto friend_id = row[0].get<int>();
			//备注是本人给好友起的名字, 允许为空; 空串表示没设, 由客户端回落到昵称或用户名
			const std::string back = row[1].isNull() ? std::string() : row[1].get<std::string>();
			//再一次查询friend_id对应的信息
			auto user_info = GetUser(friend_id);
			if (user_info == nullptr) {
				row = res.fetchOne();
				continue;
			}

			user_info->back = back;
			user_info->tags = JsonToTags(row[2].isNull() ? std::string() : row[2].get<std::string>());
			user_info_list.push_back(user_info);
			row = res.fetchOne();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error: " << e.what() << std::endl;
		return false;
	}
}

long long MysqlDao::GetOrCreatePrivateChat(int user1, int user2)
{
	//会话只对好友开放; 自己和自己不建会话
	if (user1 <= 0 || user2 <= 0 || user1 == user2)
		return 0;
	//private_chat 的唯一键要求两人按固定顺序存放, 避免 (A,B) 和 (B,A) 建出两条会话
	const int low = std::min(user1, user2);
	const int high = std::max(user1, user2);

	//会话通常已经存在: 先用普通查询取一次, 不开事务, 也不会给不存在的行加锁
	{
		auto con = pool_->getConnection();
		if (con == nullptr)
			return 0;
		Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
		try {
			auto friendship = con->_session->sql(
				"SELECT id FROM friend WHERE self_id = ? AND friend_id = ?")
				.bind(user1).bind(user2).execute().fetchOne();
			if (friendship.isNull())
				return 0;

			auto existing = con->_session->sql(
				"SELECT thread_id FROM private_chat WHERE user1_id = ? AND user2_id = ?")
				.bind(low).bind(high).execute().fetchOne();
			if (!existing.isNull())
				return existing[0].get<int64_t>(); //X DevAPI 只对 int64_t 实例化 get
		}
		catch (mysqlx::Error& e) {
			con->_healthy = false; //标记为不可复用, 归还时丢弃
			std::cerr << "Mysqlx Error reading private chat: " << e.what() << std::endl;
			return 0;
		}
	}

	//没有会话时直接插入, 由 uk_private_users 唯一键决定谁创建成功:
	//不要先 SELECT ... FOR UPDATE 占位。查不到记录时它锁的是索引间隙, 而随后的 INSERT
	//需要插入意向锁, 两个并发请求会各自持有间隙锁再等对方释放, 形成死锁(1213)。
	//这里被唯一键挡住的一方回滚后能读到对方已提交的 thread_id, 直接复用即可。
	for (int attempt = 0; attempt < 3; ++attempt) {
		auto con = pool_->getConnection();
		if (con == nullptr)
			return 0;
		try {
			con->_session->startTransaction();
			//先建公共会话行, 再挂上两人的单聊关系; 唯一键冲突时整笔回滚, 不会留下空会话
			con->_session->sql("INSERT INTO chat_thread (thread_type) VALUES (1)").execute();
			auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
			if (id_row.isNull()) {
				con->_session->rollback();
				pool_->returnConnection(std::move(con));
				return 0;
			}
			const long long thread_id = id_row[0].get<int64_t>();
			con->_session->sql("INSERT INTO private_chat (thread_id, user1_id, user2_id) VALUES (?, ?, ?)")
				.bind(thread_id).bind(low).bind(high).execute();
			con->_session->commit();
			pool_->returnConnection(std::move(con));
			return thread_id;
		}
		catch (mysqlx::Error& e) {
			//事务可能还停在中间, 先回滚; 连接已断时回滚本身会抛异常, 不能让它逃出本函数
			bool connection_ok = true;
			try { con->_session->rollback(); } catch (...) { connection_ok = false; }

			//回滚后查一次: 查到会话说明另一方已经建好, 本次失败属于正常竞态;
			//查不到才是真的失败, 换下一轮重试
			long long thread_id = 0;
			try {
				auto existing = con->_session->sql(
					"SELECT thread_id FROM private_chat WHERE user1_id = ? AND user2_id = ?")
					.bind(low).bind(high).execute().fetchOne();
				if (!existing.isNull())
					thread_id = existing[0].get<int64_t>();
			}
			catch (mysqlx::Error& read_error) {
				connection_ok = false; //连查询都失败, 这条连接不能再用
				if (attempt == 2)
					std::cerr << "Mysqlx Error re-reading private chat: " << read_error.what() << std::endl;
			}
			if (!connection_ok)
				con->_healthy = false;
			pool_->returnConnection(std::move(con));
			if (thread_id > 0)
				return thread_id;
			if (attempt == 2)
				std::cerr << "Mysqlx Error creating private chat: " << e.what() << std::endl;
		}
	}
	return 0;
}

bool MysqlDao::GetChatThreads(int uid, long long last_thread_id, int limit,
	std::vector<ChatThreadInfo>& threads, bool& has_more)
{
	threads.clear();
	has_more = false;
	//单页上限固定为100, 防止客户端传过大的值
	limit = std::clamp(limit, 1, 100);
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		//单聊与群聊共用 chat_thread 的 thread_id 游标, 一次查出两类会话。
		//单聊用 CASE 翻成"对方"视角; 群聊取群名、成员数、我的角色与免打扰;
		//未读数按会话类型分别算: 单聊看 is_read, 群聊看我的已读游标之后的消息。
		//"有人@我"只在群聊里判断, 且只看游标之后的未读消息。
		auto res = con->_session->sql(
			"SELECT t.thread_id, t.thread_type, UNIX_TIMESTAMP(t.updated_at), "
			"(pc.thread_id IS NOT NULL), "
			"CASE WHEN pc.user1_id = ? THEN pc.user2_id ELSE pc.user1_id END, "
			"u.name, u.nick, u.icon, u.sex, "
			"gc.group_name, gc.owner_id, gm.role, gm.nickname, gm.muted, "
			"(SELECT COUNT(*) FROM group_chat_member g WHERE g.thread_id = t.thread_id), "
			"(SELECT COUNT(*) FROM chat_message m "
			" WHERE m.thread_id = t.thread_id AND m.sender_id <> ? "
			" AND ((t.thread_type <> 2 AND m.recv_id = ? AND m.is_read = 0) "
			"   OR (t.thread_type = 2 AND m.message_id > gm.last_read_message_id))), "
			"(t.thread_type = 2 AND EXISTS (SELECT 1 FROM chat_message m2 "
			" WHERE m2.thread_id = t.thread_id AND m2.message_id > gm.last_read_message_id "
			" AND m2.sender_id <> ? AND FIND_IN_SET(?, m2.mentions))) "
			"FROM chat_thread t "
			"LEFT JOIN private_chat pc ON pc.thread_id = t.thread_id "
			"LEFT JOIN group_chat gc ON gc.thread_id = t.thread_id "
			"LEFT JOIN group_chat_member gm ON gm.thread_id = t.thread_id AND gm.user_id = ? "
			"LEFT JOIN user u ON u.uid = CASE WHEN pc.user1_id = ? THEN pc.user2_id ELSE pc.user1_id END "
			"WHERE t.thread_id > ? AND ((pc.thread_id IS NOT NULL AND (pc.user1_id = ? OR pc.user2_id = ?)) "
			" OR gm.thread_id IS NOT NULL) "
			"ORDER BY t.thread_id ASC LIMIT ?")
			.bind(uid).bind(uid).bind(uid).bind(uid).bind(std::to_string(uid))
			.bind(uid).bind(uid).bind(last_thread_id).bind(uid).bind(uid).bind(limit + 1).execute();
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
			}
			info.unread = row[15].isNull() ? 0 : row[15].get<int>();
			info.at_me = !row[16].isNull() && row[16].get<int>() != 0;
			threads.push_back(std::move(info));
			row = res.fetchOne();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error loading chat threads: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetChatMessages(int uid, long long thread_id, long long last_message_id, int limit,
	std::vector<ChatMessageInfo>& messages, bool& has_more)
{
	messages.clear();
	has_more = false;
	limit = std::clamp(limit, 1, 100);
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		//先确认请求者属于这个会话(单聊的两人之一或群成员), 不能靠猜 thread_id 读别人的聊天记录
		auto member = con->_session->sql(
			"SELECT thread_id FROM private_chat WHERE thread_id = ? AND (user1_id = ? OR user2_id = ?) "
			"UNION SELECT thread_id FROM group_chat_member WHERE thread_id = ? AND user_id = ?")
			.bind(thread_id).bind(uid).bind(uid).bind(thread_id).bind(uid).execute().fetchOne();
		if (member.isNull())
			return false;
		//按消息ID升序增量读取; 多取一条判断下一页
		//时间取 UNIX 时间戳: 库里是 TIMESTAMP, 直接读字符串还要处理会话时区
		auto res = con->_session->sql(
			"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, status, msg_type, "
			"UNIX_TIMESTAMP(created_at), is_read, recalled, mentions "
			"FROM chat_message WHERE thread_id = ? AND message_id > ? "
			"ORDER BY message_id ASC LIMIT ?")
			.bind(thread_id).bind(last_message_id).bind(limit + 1).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			if (static_cast<int>(messages.size()) == limit) {
				has_more = true;
				break;
			}
			ChatMessageInfo info;
			info.message_id = row[0].get<int64_t>();
			info.thread_id = row[1].get<int64_t>();
			info.sender_id = row[2].get<int>();
			info.recv_id = row[3].get<int>();
			info.client_msg_id = row[4].get<std::string>();
			info.content = row[5].get<std::string>();
			info.status = row[6].get<int>();
			info.msg_type = row[7].get<int>();
			info.created_at = row[8].get<int64_t>();
			info.is_read = row[9].get<int>() != 0;
			info.recalled = row[10].get<int>() != 0;
			info.mentions = row[11].isNull() ? "" : row[11].get<std::string>();
			messages.push_back(std::move(info));
			row = res.fetchOne();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false; //标记为不可复用, 归还时丢弃
		std::cerr << "Mysqlx Error loading chat messages: " << e.what() << std::endl;
		return false;
	}
}

//单聊会话解析: thread_id 为0时先确认好友关系再补建; 返回0表示不是好友或数据库不可用
long long MysqlDao::EnsureChatThread(int sender_id, int recv_id, long long thread_id)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return 0;
	//不管有没有会话，先确认双向关系都还在：任何一方删了好友，旧的 thread_id 都不能继续发消息
	try {
		auto friendship = con->_session->sql(
			"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
			"OR (self_id = ? AND friend_id = ?)")
			.bind(sender_id).bind(recv_id).bind(recv_id).bind(sender_id).execute().fetchOne();
		if (friendship.isNull() || friendship[0].get<int>() != 2) {
			pool_->returnConnection(std::move(con));
			return 0;
		}
	} catch (mysqlx::Error& e) {
		con->_healthy = false;
		pool_->returnConnection(std::move(con));
		std::cerr << "Mysqlx Error checking chat friendship: " << e.what() << std::endl;
		return 0;
	}
	pool_->returnConnection(std::move(con));
	if (thread_id > 0)
		return thread_id;
	return GetOrCreatePrivateChat(sender_id, recv_id); //内部会再借一次连接
}

bool MysqlDao::SaveChatMessages(int sender_id, int recv_id, long long thread_id,
	const std::vector<std::pair<std::string, std::string>>& contents,
	std::vector<ChatMessageInfo>& messages)
{
	messages.clear();
	if (contents.empty())
		return true;
	//客户端还没拿到会话ID时, 先确认两人仍是好友, 再由这里补建会话
	thread_id = EnsureChatThread(sender_id, recv_id, thread_id);
	if (thread_id <= 0)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		//写入前再校验一次会话归属, 防止客户端传一个别人的 thread_id
		auto member = con->_session->sql(
			"SELECT thread_id FROM private_chat WHERE thread_id = ? AND ((user1_id = ? AND user2_id = ?) "
			"OR (user1_id = ? AND user2_id = ?))")
			.bind(thread_id).bind(sender_id).bind(recv_id).bind(recv_id).bind(sender_id).execute().fetchOne();
		if (member.isNull())
			return false;

		//整批消息一个事务: 要么全部拿到 message_id, 要么一条都不落库
		con->_session->startTransaction();
		for (const auto& [client_id, content] : contents) {
			//(thread_id, sender_id, client_msg_id) 唯一; 断线重发同一条时沿用库里原来的 message_id
			con->_session->sql(
				"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content) "
				"VALUES (?, ?, ?, ?, ?) ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
				.bind(thread_id).bind(sender_id).bind(recv_id).bind(client_id).bind(content).execute();
			auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
			if (id_row.isNull()) {
				con->_session->rollback();
				return false;
			}
			ChatMessageInfo info;
			info.message_id = id_row[0].get<int64_t>();
			info.thread_id = thread_id;
			info.sender_id = sender_id;
			info.recv_id = recv_id;
			info.client_msg_id = client_id;
			info.content = content;
			//重发同一条时库里保留原始时间, 客户端展示与排序都按这个值
			auto time_row = con->_session->sql(
				"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
				.bind(static_cast<int64_t>(info.message_id)).execute().fetchOne();
			info.created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
			messages.push_back(std::move(info));
		}
		//会话列表按更新时间排序时需要这个时间戳, 消息写入和会话刷新放在同一事务
		con->_session->sql("UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error saving chat messages: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::SaveResourceMessage(int sender_id, int recv_id, long long thread_id,
	const std::string& client_msg_id, int msg_type, const std::string& content,
	ChatMessageInfo& message)
{
	thread_id = EnsureChatThread(sender_id, recv_id, thread_id);
	if (thread_id <= 0)
		return false;
	if (msg_type < 1 || msg_type > 3)
		return false;

	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });

	try {
		//与文本消息一样校验会话归属, 防止用别人的 thread_id 写消息
		auto member = con->_session->sql(
			"SELECT thread_id FROM private_chat WHERE thread_id = ? AND ((user1_id = ? AND user2_id = ?) "
			"OR (user1_id = ? AND user2_id = ?))")
			.bind(thread_id).bind(sender_id).bind(recv_id).bind(recv_id).bind(sender_id).execute().fetchOne();
		if (member.isNull())
			return false;

		con->_session->startTransaction();
		//status=0 表示资源还没上传完成, 由后续资源回调改成已发送；
		//content 对图片是资源名, 对视频/文件是文件名、大小、资源名与预览资源的 JSON
		con->_session->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, msg_type, status) "
			"VALUES (?, ?, ?, ?, ?, ?, 0) ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
			.bind(thread_id).bind(sender_id).bind(recv_id).bind(client_msg_id).bind(content)
			.bind(msg_type).execute();
		auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->_session->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		//重发同一条时库里保留原始时间, 这里按 message_id 读回来, 不用本地时间冒充
		auto time_row = con->_session->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		const long long created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		con->_session->sql("UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();

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
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error saving resource message: " << e.what() << std::endl;
		return false;
	}
}

//资源服务器只带回 message_id 和资源名，类型、原始文件名与预览信息统一以库里的记录为准
bool MysqlDao::GetChatMessage(long long message_id, ChatMessageInfo& message)
{
	if (message_id <= 0)
		return false;

	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });

	try {
		auto row = con->_session->sql(
			"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, status, msg_type, "
			"UNIX_TIMESTAMP(created_at), is_read, recalled "
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
		message.is_read = row[9].get<int>() != 0;
		message.recalled = row[10].get<int>() != 0;
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error getting chat message: " << e.what() << std::endl;
		return false;
	}
}


bool MysqlDao::UpdateChatMessageStatus(long long message_id, int sender, int status)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });

	try {
		//带上 sender 限定，避免拿别人的 message_id 改状态；重复回调affected rows为0仍按成功
		con->_session->sql("UPDATE chat_message SET status = ? WHERE message_id = ? AND sender_id = ?")
			.bind(status).bind(static_cast<int64_t>(message_id)).bind(sender).execute();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error updating chat message status: " << e.what() << std::endl;
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
bool LoadGroupMembers(mysqlx::Session& session, long long thread_id,
	std::vector<GroupMemberInfo>& members)
{
	members.clear();
	auto res = session.sql(
		"SELECT gm.user_id, u.name, u.nick, u.icon, u.sex, gm.role, gm.nickname, "
		"UNIX_TIMESTAMP(gm.joined_at) "
		"FROM group_chat_member gm JOIN user u ON u.uid = gm.user_id "
		"WHERE gm.thread_id = ? ORDER BY gm.role DESC, gm.joined_at ASC")
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
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto row = con->_session->sql("SELECT thread_type FROM chat_thread WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		if (row.isNull())
			return false;
		thread_type = row[0].get<int>();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error loading thread type: " << e.what() << std::endl;
		return false;
	}
}

//建群是一个事务：会话行、群资料、全部成员、首条系统消息要么都成功，要么都不留。
//邀请对象必须是群主的好友，避免出现"把陌生人拉进群"的越权路径
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

	auto con = pool_->getConnection();
	if (con == nullptr) {
		error = ErrorCodes::RPCFailed;
		return 0;
	}
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		for (int uid : invite) {
			auto row = con->_session->sql(
				"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
				"OR (self_id = ? AND friend_id = ?)")
				.bind(owner_id).bind(uid).bind(uid).bind(owner_id).execute().fetchOne();
			if (row.isNull() || row[0].get<int>() != 2) {
				error = ErrorCodes::NotFriend;
				return 0;
			}
		}

		con->_session->startTransaction();
		con->_session->sql("INSERT INTO chat_thread (thread_type) VALUES (2)").execute();
		auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->_session->rollback();
			error = ErrorCodes::RPCFailed;
			return 0;
		}
		const long long thread_id = id_row[0].get<int64_t>();
		con->_session->sql(
			"INSERT INTO group_chat (thread_id, group_name, owner_id) VALUES (?, ?, ?)")
			.bind(thread_id).bind(group_name).bind(owner_id).execute();
		con->_session->sql(
			"INSERT INTO group_chat_member (thread_id, user_id, role) VALUES (?, ?, 2)")
			.bind(thread_id).bind(owner_id).execute();
		for (int uid : invite) {
			con->_session->sql(
				"INSERT INTO group_chat_member (thread_id, user_id, role) VALUES (?, ?, 0)")
				.bind(thread_id).bind(uid).execute();
		}
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();

		info.thread_id = thread_id;
		info.name = group_name;
		info.owner_id = owner_id;
		info.member_count = static_cast<int>(invite.size()) + 1;
		info.my_role = static_cast<int>(GroupRole::Owner);
		info.created_at = std::time(nullptr);
		LoadGroupMembers(*con->_session, thread_id, member_list);
		return thread_id;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error creating group: " << e.what() << std::endl;
		error = ErrorCodes::RPCFailed;
		return 0;
	}
}

bool MysqlDao::GetGroupInfo(int uid, long long thread_id, GroupInfo& info,
	std::vector<GroupMemberInfo>& members)
{
	members.clear();
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		//JOIN 成员行同时完成"是不是群成员"的校验，非成员查不到任何行
		auto row = con->_session->sql(
			"SELECT gc.group_name, gc.notice, gc.owner_id, UNIX_TIMESTAMP(gc.created_at), "
			"(SELECT COUNT(*) FROM group_chat_member g WHERE g.thread_id = gc.thread_id), "
			"gm.role, gm.muted, gm.nickname, gm.last_read_message_id "
			"FROM group_chat gc JOIN group_chat_member gm "
			"ON gm.thread_id = gc.thread_id AND gm.user_id = ? "
			"WHERE gc.thread_id = ?")
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
		LoadGroupMembers(*con->_session, thread_id, members);
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error loading group info: " << e.what() << std::endl;
		return false;
	}
}

//群设置更新：群名与公告需要管理员及以上；群昵称与免打扰只改自己那一行。
//群昵称走 member 表，不碰 user 表，避免"改个群备注把账号昵称也改了"
int MysqlDao::UpdateGroupField(long long thread_id, int uid, GroupField field,
	const std::string& value, std::string& applied)
{
	applied = value;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		int role = -1;
		bool muted = false;
		long long last_read = 0;
		GroupMemberInfo member;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, uid, member, muted, last_read))
			return ErrorCodes::GroupNotFound;
		role = member.role;

		if (field == GroupField::Name || field == GroupField::Notice) {
			if (role < static_cast<int>(GroupRole::Admin))
				return ErrorCodes::GroupDenied;
			if (field == GroupField::Name) {
				con->_session->sql("UPDATE group_chat SET group_name = ?, "
					"updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
					.bind(value).bind(thread_id).execute();
			}
			else {
				con->_session->sql("UPDATE group_chat SET notice = ?, "
					"updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
					.bind(value).bind(thread_id).execute();
			}
			return ErrorCodes::Success;
		}

		if (field == GroupField::MyNickname) {
			con->_session->sql(
				"UPDATE group_chat_member SET nickname = ? WHERE thread_id = ? AND user_id = ?")
				.bind(value).bind(thread_id).bind(uid).execute();
			return ErrorCodes::Success;
		}

		const bool target = value == "1";
		con->_session->sql(
			"UPDATE group_chat_member SET muted = ? WHERE thread_id = ? AND user_id = ?")
			.bind(target ? 1 : 0).bind(thread_id).bind(uid).execute();
		applied = target ? "1" : "0";
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error updating group field: " << e.what() << std::endl;
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
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;

		auto count_row = con->_session->sql(
			"SELECT COUNT(*) FROM group_chat_member WHERE thread_id = ?")
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
			auto exists = con->_session->sql(
				"SELECT COUNT(*) FROM group_chat_member WHERE thread_id = ? AND user_id = ?")
				.bind(thread_id).bind(uid).execute().fetchOne();
			if (!exists.isNull() && exists[0].get<int>() > 0)
				continue;
			candidates.push_back(uid);
		}
		if (candidates.empty())
			return ErrorCodes::Success;

		con->_session->startTransaction();
		for (int uid : candidates) {
			auto friend_row = con->_session->sql(
				"SELECT COUNT(*) FROM friend WHERE (self_id = ? AND friend_id = ?) "
				"OR (self_id = ? AND friend_id = ?)")
				.bind(operator_uid).bind(uid).bind(uid).bind(operator_uid).execute().fetchOne();
			if (friend_row.isNull() || friend_row[0].get<int>() != 2)
				continue; //不是好友的人静默跳过，由调用方按 added 结果决定提示
			if (member_count >= MAX_GROUP_MEMBER)
				break;
			con->_session->sql(
				"INSERT INTO group_chat_member (thread_id, user_id, role) VALUES (?, ?, 0)")
				.bind(thread_id).bind(uid).execute();
			added.push_back(uid);
			++member_count;
		}
		if (added.empty()) {
			con->_session->rollback();
			return ErrorCodes::NotFriend;
		}
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error inviting group members: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

int MysqlDao::RemoveGroupMember(long long thread_id, int operator_uid, int target_uid,
	int& member_count)
{
	member_count = 0;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		GroupMemberInfo target;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, target_uid, target, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (target.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;
		//管理员只能移除普通成员，不能互相踢；群主不受这条限制
		if (self.role == static_cast<int>(GroupRole::Admin)
			&& target.role >= static_cast<int>(GroupRole::Admin))
			return ErrorCodes::GroupDenied;
		if (self.role < static_cast<int>(GroupRole::Admin))
			return ErrorCodes::GroupDenied;

		con->_session->startTransaction();
		con->_session->sql(
			"DELETE FROM group_chat_member WHERE thread_id = ? AND user_id = ?")
			.bind(thread_id).bind(target_uid).execute();
		auto count_row = con->_session->sql(
			"SELECT COUNT(*) FROM group_chat_member WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		member_count = count_row.isNull() ? 0 : count_row[0].get<int>();
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error removing group member: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

//设/撤管理员只有群主可以操作；目标必须是普通成员或管理员，不能是群主自己
int MysqlDao::SetGroupAdmin(long long thread_id, int operator_uid, int target_uid, bool admin)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		GroupMemberInfo target;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, operator_uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (self.role != static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, target_uid, target, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (target.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;

		const int role = admin ? static_cast<int>(GroupRole::Admin)
			: static_cast<int>(GroupRole::Member);
		con->_session->sql(
			"UPDATE group_chat_member SET role = ? WHERE thread_id = ? AND user_id = ?")
			.bind(role).bind(thread_id).bind(target_uid).execute();
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error setting group admin: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

//退群：群主不能直接退，必须先解散或（后续版本）转让；普通成员与管理员删掉自己那一行
int MysqlDao::LeaveGroup(long long thread_id, int uid, bool& dismissed)
{
	dismissed = false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, uid, self, muted, last_read))
			return ErrorCodes::GroupNotFound;
		if (self.role == static_cast<int>(GroupRole::Owner))
			return ErrorCodes::GroupDenied;

		con->_session->startTransaction();
		con->_session->sql("DELETE FROM group_chat_member WHERE thread_id = ? AND user_id = ?")
			.bind(thread_id).bind(uid).execute();
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error leaving group: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

//解散：删 chat_thread，group_chat / group_chat_member / chat_message 按外键级联清理
int MysqlDao::DismissGroup(long long thread_id, int uid)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return ErrorCodes::RPCFailed;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto row = con->_session->sql(
			"SELECT owner_id FROM group_chat WHERE thread_id = ?")
			.bind(thread_id).execute().fetchOne();
		if (row.isNull())
			return ErrorCodes::GroupNotFound;
		if (row[0].get<int>() != uid)
			return ErrorCodes::GroupDenied;

		con->_session->sql("DELETE FROM chat_thread WHERE thread_id = ?")
			.bind(thread_id).execute();
		return ErrorCodes::Success;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error dismissing group: " << e.what() << std::endl;
		return ErrorCodes::RPCFailed;
	}
}

bool MysqlDao::AddGroupSystemMessage(long long thread_id, int actor,
	const std::string& content, ChatMessageInfo& message)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		con->_session->startTransaction();
		con->_session->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
			"msg_type, status, is_read, mentions) VALUES (?, ?, 0, ?, ?, 4, 1, 1, '')")
			.bind(thread_id).bind(actor).bind(NextSystemMsgId(thread_id)).bind(content).execute();
		auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->_session->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		auto time_row = con->_session->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();

		message.message_id = message_id;
		message.thread_id = thread_id;
		message.sender_id = actor;
		message.recv_id = 0;
		message.content = content;
		message.msg_type = 4;
		message.status = 1;
		message.is_read = true;
		message.created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error adding group system message: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetGroupMemberIds(long long thread_id, std::vector<int>& members)
{
	members.clear();
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		auto res = con->_session->sql(
			"SELECT user_id FROM group_chat_member WHERE thread_id = ? ORDER BY joined_at ASC")
			.bind(thread_id).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			members.push_back(row[0].get<int>());
			row = res.fetchOne();
		}
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error loading group members: " << e.what() << std::endl;
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
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, sender_id, self, muted, last_read))
			return false;

		//整批消息一个事务：幂等键与单聊一致，(thread_id, sender_id, client_msg_id) 唯一
		con->_session->startTransaction();
		for (const auto& [client_id, content] : contents) {
			con->_session->sql(
				"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
				"msg_type, status, mentions) VALUES (?, ?, 0, ?, ?, 0, 1, ?) "
				"ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
				.bind(thread_id).bind(sender_id).bind(client_id).bind(content)
				.bind(mentions).execute();
			auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
			if (id_row.isNull()) {
				con->_session->rollback();
				return false;
			}
			ChatMessageInfo info;
			info.message_id = id_row[0].get<int64_t>();
			info.thread_id = thread_id;
			info.sender_id = sender_id;
			info.recv_id = 0;
			info.client_msg_id = client_id;
			info.content = content;
			info.msg_type = 0;
			info.status = 1;
			info.mentions = mentions;
			auto time_row = con->_session->sql(
				"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
				.bind(static_cast<int64_t>(info.message_id)).execute().fetchOne();
			info.created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
			messages.push_back(std::move(info));
		}
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error saving group messages: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::SaveGroupResourceMessage(int sender_id, long long thread_id,
	const std::string& mentions, const std::string& client_msg_id, int msg_type,
	const std::string& content, ChatMessageInfo& message)
{
	if (msg_type < 1 || msg_type > 3)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		GroupMemberInfo self;
		bool muted = false;
		long long last_read = 0;
		if (!GetGroupMemberOn(con->_session.get(), thread_id, sender_id, self, muted, last_read))
			return false;

		con->_session->startTransaction();
		con->_session->sql(
			"INSERT INTO chat_message (thread_id, sender_id, recv_id, client_msg_id, content, "
			"msg_type, status, mentions) VALUES (?, ?, 0, ?, ?, ?, 0, ?) "
			"ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)")
			.bind(thread_id).bind(sender_id).bind(client_msg_id).bind(content)
			.bind(msg_type).bind(mentions).execute();
		auto id_row = con->_session->sql("SELECT LAST_INSERT_ID()").execute().fetchOne();
		if (id_row.isNull()) {
			con->_session->rollback();
			return false;
		}
		const long long message_id = id_row[0].get<int64_t>();
		auto time_row = con->_session->sql(
			"SELECT UNIX_TIMESTAMP(created_at) FROM chat_message WHERE message_id = ?")
			.bind(static_cast<int64_t>(message_id)).execute().fetchOne();
		con->_session->sql(
			"UPDATE chat_thread SET updated_at = CURRENT_TIMESTAMP WHERE thread_id = ?")
			.bind(thread_id).execute();
		con->_session->commit();

		message.message_id = message_id;
		message.thread_id = thread_id;
		message.sender_id = sender_id;
		message.recv_id = 0;
		message.client_msg_id = client_msg_id;
		message.content = content;
		message.msg_type = msg_type;
		message.status = 0;
		message.created_at = time_row.isNull() ? 0 : time_row[0].get<int64_t>();
		message.mentions = mentions;
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) {
			try { con->_session->rollback(); } catch (...) {}
			con->_healthy = false;
		}
		std::cerr << "Mysqlx Error saving group resource message: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::MarkGroupRead(int uid, long long thread_id, long long last_message_id)
{
	if (uid <= 0 || thread_id <= 0 || last_message_id <= 0)
		return false;
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		//GREATEST 保证游标只前进：乱序到达的旧"已读"不会把未读数又拉回来
		auto result = con->_session->sql(
			"UPDATE group_chat_member SET last_read_message_id = "
			"GREATEST(last_read_message_id, ?) WHERE thread_id = ? AND user_id = ?")
			.bind(static_cast<int64_t>(last_message_id)).bind(thread_id).bind(uid).execute();
		if (result.getAffectedItemsCount() > 0)
			return true;
		//值没变化时 affected 为 0，仍要确认是群成员，不能把"不是成员"当成成功
		GroupMemberInfo member;
		bool muted = false;
		long long last_read = 0;
		return GetGroupMemberOn(con->_session.get(), thread_id, uid, member, muted, last_read);
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error marking group read: " << e.what() << std::endl;
		return false;
	}
}

bool MysqlDao::GetGroupMember(long long thread_id, int uid, GroupMemberInfo& member,
	bool& muted, long long& last_read_message_id)
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;
	Defer defer([this, &con]() { pool_->returnConnection(std::move(con)); });
	try {
		return GetGroupMemberOn(con->_session.get(), thread_id, uid, member, muted,
			last_read_message_id);
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "Mysqlx Error loading group member: " << e.what() << std::endl;
		return false;
	}
}

//同一连接内复用：群资料、权限判断、消息写入都要先确认"我是不是这个群的成员"
bool MysqlDao::GetGroupMemberOn(mysqlx::Session* session, long long thread_id, int uid,
	GroupMemberInfo& member, bool& muted, long long& last_read_message_id)
{
	muted = false;
	last_read_message_id = 0;
	auto row = session->sql(
		"SELECT gm.user_id, u.name, u.nick, u.icon, u.sex, gm.role, gm.nickname, "
		"gm.muted, gm.last_read_message_id, UNIX_TIMESTAMP(gm.joined_at) "
		"FROM group_chat_member gm JOIN user u ON u.uid = gm.user_id "
		"WHERE gm.thread_id = ? AND gm.user_id = ?")
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

MySqlPool::MySqlPool(const std::string& host, unsigned port, const std::string& user, const std::string& pass,
	const std::string& schema, int poolSize)
    : host_(host), port_(port), user_(user), pass_(pass), schema_(schema), poolSize_(poolSize), b_stop_(false)
{
	//逐条建立连接, 单条失败不影响其余连接; 数量缺口由检测线程首轮补足
	for (int i = 0; i < poolSize_; ++i) {
		try {
			mysqlx::SessionSettings settings(host_, port_, user_, pass_, schema_);
			auto session = std::make_unique<mysqlx::Session>(settings);
			auto currentTime = std::chrono::system_clock::now().time_since_epoch();
			long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(currentTime).count();
			pool_.push(std::make_unique<SqlConnection>(std::move(session), timestamp));
			++connection_count_;
		}
		catch (mysqlx::Error& e) {
			std::cout << "mysql connection init failed: " << e.what() << std::endl;
		}
	}

    //启动检测线程, 定期检查连接的状态并保持连接的活跃性(心跳机制)
	_check_thread = std::thread([this]() {
		while (!b_stop_) {
			//锁只覆盖等待本身, 检查与重连全部在锁外执行
			{
				std::unique_lock<std::mutex> lock(mutex_);
				//两个唤醒来源: 停机, 或业务线程丢弃连接后请求补池; 谓词在锁内求值, 不会漏掉通知
				check_cond_.wait_for(lock, std::chrono::seconds(60), [this] {
					return b_stop_.load() || check_requested_;
				});
				if (b_stop_) break;
				check_requested_ = false; //在锁内清除, 与置位方互斥, 本轮未处理的缺口仍会在本轮补足
			}
			checkConnection(); //检查连接的状态并保持连接的活跃性(心跳机制)
		}
	});
}

void MySqlPool::checkConnection() {
    //获取当前连接池中连接的数量, 以便在遍历连接池时不受其他线程影响
    size_t targetCount;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        targetCount = pool_.size();
    }

    size_t processed = 0; //当前已经处理的数量

    //获取当前时间戳
    auto now = std::chrono::system_clock::now().time_since_epoch();
    long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    while (processed < targetCount) {
        std::unique_ptr<SqlConnection> con; //获取连接对象
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (pool_.empty())
                break;

            con = std::move(pool_.front());
            pool_.pop();
        }

        bool healthy = true;
        //解锁后做检查/重连逻辑
        if (timestamp - con->_last_oper_time >= 5) {
            try {
                con->_session->sql("SELECT 1").execute(); //执行简单查询以保持连接活跃
                con->_last_oper_time = timestamp; //更新最后操作时间
            }
            catch (mysqlx::Error& e) {
                std::cout << "Error keeping connection alive: " << e.what() << std::endl;
                healthy = false;
            }
        }

        //如果连接健康, 则将其归还给连接池; 否则让局部对象析构关闭它, 由本函数末尾的重连补足
        if (healthy)
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (b_stop_) {
                --connection_count_;
            } else {
                pool_.push(std::move(con));
                cond_.notify_one();
            }
        } else {
            std::lock_guard<std::mutex> guard(mutex_);
            --connection_count_;
        }
        ++processed; //更新已处理的数量
    }

    while (!b_stop_) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            //补足到池容量: 覆盖检查失败、业务线程丢弃、启动时建连失败三种缺口
            if (connection_count_ >= static_cast<size_t>(poolSize_)) break;
        }
        if (!reconnect(timestamp))
            break; //数据库不可达时不在锁外空转, 留到下一个周期再试
    }
}

bool MySqlPool::reconnect(long long timestamp) {
    try {
        //重新创建会话对象并替换旧的会话
        mysqlx::SessionSettings settings(host_, port_, user_, pass_, schema_);
        //X Protocol 下 caching_sha2_password 快速认证需要 TLS, 使用默认 SSL_MODE=REQUIRED(本地开发可接受)
        auto session = std::make_unique<mysqlx::Session>(settings); //替换旧的会话对象

        auto newCon = std::make_unique<SqlConnection>(std::move(session), timestamp);
        {
            std::lock_guard<std::mutex> guard(mutex_);
            //如果连接池被标记为停止, 则直接返回false, 不将新连接对象加入连接池
            if (b_stop_)
                return false;
            pool_.push(std::move(newCon)); //将新连接对象加入连接池队列, 使其可供其他线程使用
            ++connection_count_; //新增连接是池容量增长的唯一来源, 必须与队列同步记账
        }
        cond_.notify_one(); //通知一个等待的线程, 使其从连接池中获取一个可用的连接对象
        std::cout << "mysql connection reconnect success" << std::endl;
        return true;
    }
    catch (mysqlx::Error& e) {
        std::cout << "Reconnect failed, error is " << e.what() << std::endl;
        return false;
    }
}

std::unique_ptr<SqlConnection> MySqlPool::getConnection()
{
	std::unique_lock<std::mutex> lock(mutex_); //加锁保护连接池的状态
	//使用条件变量等待连接池中有可用的连接或者连接池被标记为停止
	cond_.wait(lock, [this] {
		//如果连接池被标记为停止, 则直接返回true, 保持锁定状态并继续执行
		if (b_stop_) {
			return true;
		}
		//如果没有可用的连接, 返回false, 解锁并进入等待状态;
		//如果有可用的连接, 返回true, 保持锁定状态并继续执行
		return !pool_.empty(); });
	//如果连接池被标记为停止, 直接返回空指针
	if (b_stop_) {
		return nullptr;
	}
	//从连接池中获取一个可用的连接对象
	std::unique_ptr<SqlConnection> con(std::move(pool_.front()));
	pool_.pop(); //将连接对象从连接池队列中移除
	return con;
}

void MySqlPool::returnConnection(std::unique_ptr<SqlConnection> con)
{
	if (!con) return;
	std::unique_lock<std::mutex> lock(mutex_); //加锁保护连接池的状态
	//如果连接池被标记为停止, 则直接返回, 不将连接对象归还给连接池
	if (b_stop_)
		return;
	if (!con->_healthy || !con->_session) {
		//不可复用的连接不回到队列: 先按"空闲+借出"的总数销账, 再请求检测线程补建
		--connection_count_;
		check_requested_ = true;
		lock.unlock(); //关闭会话可能涉及网络收尾, 放到锁外执行
		check_cond_.notify_one();
		return;
	}

	//归还时刷新活跃时间, 检测线程据此跳过刚用过的连接, 只对真正空闲的连接做心跳
	auto now = std::chrono::system_clock::now().time_since_epoch();
	con->_last_oper_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();
	pool_.push(std::move(con)); //将连接对象加入连接池队列, 使其可供其他线程使用
	cond_.notify_one(); //通知一个等待的线程, 使其从连接池中获取一个可用的连接对象
}

void MySqlPool::Close()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		b_stop_ = true; //设置停止标志, 使连接池进入停止状态
	}
	cond_.notify_all(); //唤醒等待连接的业务线程
	check_cond_.notify_all(); //唤醒周期检测线程
	//等待检测线程退出, 确保后续清理不会与检测线程并发访问连接池
	if (_check_thread.joinable())
		_check_thread.join();

	{
		std::lock_guard<std::mutex> lock(mutex_);
		//检测线程已退出, 队列中的连接不会再被取用, 直接随容器析构关闭
		while (!pool_.empty())
			pool_.pop();
		connection_count_ = 0; //停止后容量计数不再有意义, 清零避免被误读
	}
}

MySqlPool::~MySqlPool()
{
	Close(); //幂等: 调用方已关闭时这里只做一次空转, 保证检测线程一定被回收
}
