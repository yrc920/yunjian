#include "MysqlDao.h"
#include "ConfigMgr.h"
#include "const.h"
#include <cstdlib>
#include <chrono>
#include <iostream>
#include "Log.h"

MysqlDao::MysqlDao()
{
	//连接参数从 [Mysql] 段读：Host/Port/User/Passwd/Schema，可选 QueryTimeoutMs/PoolSize
	//6 条长连接由后台线程巡检
	pool_.reset(new MysqlPool(MysqlPoolOptionsFromConfig(6, "ResourceServer")));
}

MysqlDao::~MysqlDao()
{
	pool_->Close(); //先停巡检线程再释放连接，避免析构时还有线程在借连接
}

std::shared_ptr<UserInfo> MysqlDao::GetUser(int uid)
{
	auto con = pool_->Acquire(); //从池里借一条会话
	if (!con)
		return nullptr; //连接池已停


	try {
		auto res = con->sql(
			"SELECT uid, name, pwd, email, nick, `desc`, sex, icon FROM user WHERE uid = ?") //只查需要的列
			.bind(uid).execute();
		auto row = res.fetchOne(); //按uid查询最多一行
		if (row.isNull())
			return nullptr; //用户不存在

		auto info = std::make_shared<UserInfo>(); //按列顺序回填
		info->uid = row[0].get<int>();
		info->name = row[1].get<std::string>();
		info->pwd = row[2].get<std::string>(); //缓存刷新不返回给客户端
		info->email = row[3].get<std::string>();
		info->nick = row[4].get<std::string>();
		info->desc = row[5].get<std::string>();
		info->sex = row[6].get<int>();
		info->icon = row[7].get<std::string>(); //刚写入的头像文件名
		return info;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error in GetUser: " << e.what();
		return nullptr;
	}
}

bool MysqlDao::UpdateUserIcon(int uid, const std::string& icon)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		con->sql("UPDATE user SET icon = ? WHERE uid = ?") //只改头像字段
			.bind(icon).bind(uid).execute();
		//同一个头像重复提交时affected rows为0，但语句本身成功，按成功返回
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error in UpdateUserIcon: " << e.what();
		return false;
	}
}

bool MysqlDao::CanAccessResource(int requester, int owner, const std::string& name)
{
	if (requester <= 0 || owner <= 0 || name.empty())
		return false;
	if (requester == owner)
		return true; //自己的文件不用再查

	auto con = pool_->Acquire(); //从池里借一条会话
	if (!con)
		return false; //池不可用时按拒绝处理：宁可下载失败，也不越权放行


	try {
		//头像是公开资源：只要这个文件仍是对方的当前头像，任何登录用户都能取
		auto icon = con->sql("SELECT 1 FROM user WHERE uid = ? AND icon = ?")
			.bind(owner).bind(name).execute().fetchOne();
		if (!icon.isNull())
			return true;

		//其余资源要求双方至少共处一个仍然有效的会话：
		//单聊两人各一行成员记录，群聊同 thread_id；已解散的群不再放行
		auto shared = con->sql(
			"SELECT 1 FROM conversation_member mine "
			"JOIN conversation c ON c.thread_id = mine.thread_id AND c.state = 0 "
			"JOIN conversation_member peer ON peer.thread_id = mine.thread_id "
			"WHERE mine.uid = ? AND peer.uid = ? LIMIT 1")
			.bind(requester).bind(owner).execute().fetchOne();
		return !shared.isNull();
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error checking resource access: " << e.what();
		return false;
	}
}

bool MysqlDao::UpdateChatMessageStatus(long long message_id, int sender, int upload_state)
{
	auto con = pool_->Acquire();
	if (!con)
		return false;


	try {
		//gRPC 不可用时的兜底路径，同样带上 sender 限定。
		//列名是 upload_state：status 只存在于客户端消息模型，库里没有这一列
		con->sql("UPDATE chat_message SET upload_state = ? WHERE message_id = ? AND sender_id = ?")
			.bind(upload_state).bind(static_cast<int64_t>(message_id)).bind(sender).execute();
		return true;
	}
	catch (MysqlError& e) {
		LOG_ERROR << "mysql error in UpdateChatMessageStatus: " << e.what();
		return false;
	}
}
