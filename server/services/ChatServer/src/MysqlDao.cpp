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
	EnsureFriendTagColumns();
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

//好友表与申请表各补一列 tags.json: 先查 information_schema, 缺列才 ALTER, 重复启动不会报错
bool MysqlDao::EnsureFriendTagColumns()
{
	auto con = pool_->getConnection();
	if (con == nullptr)
		return false;

	Defer defer([this, &con]() {
		pool_->returnConnection(std::move(con));
		});

	try {
		//表名是固定字面量, 不来自外部输入
		auto ensure = [&con](const std::string& table) {
			auto res = con->_session->sql(
				"SELECT COUNT(*) FROM information_schema.columns "
				"WHERE table_schema = DATABASE() AND table_name = ? AND column_name = 'tags'")
				.bind(table).execute().fetchOne();
			if (!res.isNull() && res[0].get<int>() > 0)
				return;
			//512 字节够 10 个 12 字的中文标签再加 JSON 符号
			con->_session->sql("ALTER TABLE " + table +
				" ADD COLUMN tags VARCHAR(512) NOT NULL DEFAULT '[]'").execute();
		};
		ensure("friend");
		ensure("friend_apply");
		return true;
	}
	catch (mysqlx::Error& e) {
		if (con) con->_healthy = false;
		std::cerr << "ensure tags columns failed: " << e.what() << std::endl;
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
		//申请留言、备注和分组标签都保存在申请记录里，同意时一次性复制到好友关系行
		con->_session->sql(
			"INSERT INTO friend_apply (from_uid, to_uid, descs, back_name, tags) "
			"VALUES (?, ?, ?, ?, ?) "
			"ON DUPLICATE KEY UPDATE descs = VALUES(descs), back_name = VALUES(back_name), "
			"tags = VALUES(tags)")
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
	std::string& reverse_back, std::vector<std::string>& reverse_tags) {
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
		auto res = con->_session->sql("select apply.from_uid, apply.status, user.name, user.nick, user.sex, user.icon, apply.descs "
			"from friend_apply as apply join user on apply.from_uid = user.uid where apply.to_uid = ? "
			"and apply.id > ? order by apply.id ASC LIMIT ? ")
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
			"select apply.to_uid, apply.status, user.name, user.nick, user.sex, user.icon, apply.descs "
			"from friend_apply as apply join user on apply.to_uid = user.uid where apply.from_uid = ? "
			"and apply.id > ? order by apply.id ASC LIMIT ? ")
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
			applyList.push_back(std::make_shared<ApplyInfo>(uid, name, desc, icon, nick, sex, status));
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
		//CASE 把会话行翻成"对方"视角: 自己是 user1 就取 user2, 反之取 user1
		//多取一条用来判断还有没有下一页, 返回前再截掉
		auto res = con->_session->sql(
			"SELECT pc.thread_id, CASE WHEN pc.user1_id = ? THEN pc.user2_id ELSE pc.user1_id END, "
			"u.name, u.nick, u.icon, u.sex "
			"FROM private_chat pc JOIN user u ON u.uid = CASE WHEN pc.user1_id = ? THEN pc.user2_id ELSE pc.user1_id END "
			"WHERE (pc.user1_id = ? OR pc.user2_id = ?) AND pc.thread_id > ? "
			"ORDER BY pc.thread_id ASC LIMIT ?")
			.bind(uid).bind(uid).bind(uid).bind(uid).bind(last_thread_id).bind(limit + 1).execute();
		auto row = res.fetchOne();
		while (!row.isNull()) {
			if (static_cast<int>(threads.size()) == limit) {
				has_more = true;
				break;
			}
			ChatThreadInfo info;
			info.thread_id = row[0].get<int64_t>();
			info.peer_uid = row[1].get<int>();
			info.peer_name = row[2].get<std::string>();
			info.peer_nick = row[3].get<std::string>();
			info.peer_icon = row[4].get<std::string>();
			info.peer_sex = row[5].get<int>();
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
		//先确认请求者属于这个会话, 不能靠猜 thread_id 读别人的聊天记录
		auto member = con->_session->sql(
			"SELECT thread_id FROM private_chat WHERE thread_id = ? AND (user1_id = ? OR user2_id = ?)")
			.bind(thread_id).bind(uid).bind(uid).execute().fetchOne();
		if (member.isNull())
			return false;
		//按消息ID升序增量读取; 多取一条判断下一页
		auto res = con->_session->sql(
			"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, status, msg_type "
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
	if (thread_id > 0)
		return thread_id;

	auto con = pool_->getConnection();
	if (con == nullptr)
		return 0;
	try {
		auto friendship = con->_session->sql(
			"SELECT id FROM friend WHERE self_id = ? AND friend_id = ?")
			.bind(sender_id).bind(recv_id).execute().fetchOne();
		if (friendship.isNull()) {
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
			"SELECT message_id, thread_id, sender_id, recv_id, client_msg_id, content, status, msg_type "
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
