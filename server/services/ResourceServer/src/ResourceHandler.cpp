#include "ResourceHandler.h"
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "LogicSystem.h"
#include "RedisMgr.h"
#include "const.h"
#include <boost/filesystem.hpp>
#include <json/json.h>
#include <cctype>
#include <iostream>

namespace {

bool ParseJson(const std::string& data, Json::Value& root)
{
	Json::Reader reader;
	if (!reader.parse(data, root))
		return false;
	return root.isObject();
}

bool IsMd5(const std::string& md5)
{
	if (md5.size() != 32)
		return false;
	for (char ch : md5) {
		if (!std::isxdigit(static_cast<unsigned char>(ch)))
			return false;
	}
	return true;
}

bool IsSafeName(const std::string& name)
{
	if (name.empty() || name.size() > 128)
		return false;
	if (name == "." || name == "..")
		return false;
	if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos)
		return false;
	if (name.front() == '.')
		return false;
	return boost::filesystem::path(name).filename().string() == name;
}

//返回ErrorCodes，用来区分“uid没有登录令牌”和“令牌不匹配”
int ValidateToken(int uid, const std::string& token)
{
	if (uid <= 0 || token.empty())
		return ErrorCodes::UidInvalid;

	std::string value;
	if (!RedisMgr::GetInstance()->Get(USERTOKENPREFIX + std::to_string(uid), value))
		return ErrorCodes::UidInvalid; //Redis里没有这个uid的令牌，token无从比对
	return value == token ? ErrorCodes::Success : ErrorCodes::TokenInvalid;
}

//首个请求校验token并把uid绑到连接，后续分片只比对连接身份
int EnsureSessionUser(const std::shared_ptr<CSession>& session, int uid, const std::string& token)
{
	if (uid <= 0)
		return ErrorCodes::UidInvalid;

	const int bound_uid = session->GetUserId();
	if (bound_uid != 0)
		return bound_uid == uid ? ErrorCodes::Success : ErrorCodes::TokenInvalid;

	const int error = ValidateToken(uid, token);
	if (error != ErrorCodes::Success)
		return error;
	return session->BindUser(uid) ? ErrorCodes::Success : ErrorCodes::TokenInvalid;
}

void SendUploadResponse(const std::shared_ptr<CSession>& session, bool avatar, int error,
	const std::string& md5, const std::string& name, int seq, long long total_size,
	long long trans_size, int last, int uid)
{
	Json::Value rtvalue;
	rtvalue["error"] = error;
	rtvalue["md5"] = md5;
	rtvalue["name"] = name;
	rtvalue["seq"] = seq;
	rtvalue["total_size"] = Json::Int64(total_size);
	rtvalue["trans_size"] = Json::Int64(trans_size);
	rtvalue["last"] = last;
	rtvalue["uid"] = uid;

	session->Send(rtvalue.toStyledString(),
		avatar ? ID_UPLOAD_HEAD_ICON_RSP : ID_UPLOAD_FILE_RSP);
}

}

void resource::HandleUpload(std::shared_ptr<CSession> session, const std::string& data, bool avatar)
{
	Json::Value root;
	if (!ParseJson(data, root)) { //不是合法JSON对象
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}

	//类型必须逐项检查：JsonCpp的asInt()会把字符串转成0，不能先取值再判断
	if (!root["md5"].isString() || !root["name"].isString() || !root["data"].isString() ||
		!root["seq"].isInt() || !root["trans_size"].isInt64() || !root["total_size"].isInt64() ||
		!root["last"].isInt() || !root["uid"].isInt() || !root["token"].isString()) {
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}

	//聊天图片额外带消息信息；头像与普通文件不带，缺省为0
	if ((root.isMember("message_id") && !root["message_id"].isInt64())
		|| (root.isMember("touid") && !root["touid"].isInt())
		|| (root.isMember("thread_id") && !root["thread_id"].isInt64())
		|| (root.isMember("unique_id") && !root["unique_id"].isString())
		|| (root.isMember("thread_type") && !root["thread_type"].isInt())) {
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}
	const long long chat_msg_id = root.get("message_id", Json::Int64(0)).asInt64();
	const int touid = root.get("touid", 0).asInt();
	const long long chat_thread_id = root.get("thread_id", Json::Int64(0)).asInt64();
	const std::string unique_id = root.get("unique_id", "").asString();
	//群聊资源上传带 2，资源服务器据此把完成通知发到发送者所在实例再广播
	const int thread_type = root.get("thread_type", 1).asInt() == 2 ? 2 : 1;

	const std::string md5 = root["md5"].asString(); //资源内容ID，也是续传键
	const std::string name = root["name"].asString(); //落盘文件名
	const std::string token = root["token"].asString(); //首片校验用
	const int uid = root["uid"].asInt(); //上传者
	const int seq = root["seq"].asInt(); //分片序号，从1开始
	const long long total_size = root["total_size"].asInt64(); //文件总大小
	const long long trans_size = root["trans_size"].asInt64(); //本片结束后的偏移
	const int last = root["last"].asInt(); //是否最后一片

	if (!IsMd5(md5) || !IsSafeName(name)) { //资源名或文件名不合法
		SendUploadResponse(session, avatar, ErrorCodes::FileNameInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	if (total_size <= 0 || total_size > MAX_FILE_SIZE) { //空文件或超过4GB
		SendUploadResponse(session, avatar, ErrorCodes::FileTooLarge, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	//trans_size必须与seq对应的偏移一致；last只能出现在最后一片
	if (seq < 1 || trans_size <= 0 || trans_size > total_size ||
		(last == 1) != (trans_size == total_size)) {
		SendUploadResponse(session, avatar, ErrorCodes::FileDataInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	const int auth_error = EnsureSessionUser(session, uid, token); //首片校验token，后续校验连接绑定
	if (auth_error != ErrorCodes::Success) {
		SendUploadResponse(session, avatar, auth_error, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	//头像与通用文件都按用户分目录：下载时拿 owner_uid 就能拼出同一路径
	auto storage = ConfigMgr::Inst().GetStoragePath();
	boost::filesystem::path file_path = storage / std::to_string(uid) / name;

	//第一片建立进度记录：路径、总大小、是否头像都在此时固定下来
	std::shared_ptr<FileInfo> file_info;
	if (seq == 1) {
		file_info = std::make_shared<FileInfo>();
		file_info->_md5 = md5; //续传键
		file_info->_name = name; //落盘文件名
		file_info->_file_path = file_path.string(); //绝对或相对存储路径
		file_info->_uid = uid; //归属用户
		file_info->_total_size = total_size; //总大小
		file_info->_avatar = avatar; //最后一片是否写user.icon
		file_info->_accepted_seq = 1; //首片已被逻辑线程接收
		LogicSystem::GetInstance()->AddFileInfo(file_info); //登记到内存进度表
	}
	else {
		//内存没有就查Redis；换连接续传依赖这份记录
		if (!LogicSystem::GetInstance()->LoadFileInfo(md5, file_info)) {
			SendUploadResponse(session, avatar, ErrorCodes::FileNotExists, md5, name, seq,
				total_size, trans_size, last, uid);
			return;
		}

		//续传必须沿用第一次请求确定的路径与总大小，防止同名请求覆盖别的文件
		if (file_info->_name != name || file_info->_uid != uid ||
			file_info->_total_size != total_size || file_info->_avatar != avatar) {
			SendUploadResponse(session, avatar, ErrorCodes::FileDataInvalid, md5, name, seq,
				total_size, trans_size, last, uid);
			return;
		}

		//通用资源走拥塞窗口，多个分片可能在途；按“已接收序号”而不是落盘序号判断，
		//落盘由同一文件的文件线程按队列顺序完成，不会写出空洞
		if (seq != file_info->_accepted_seq + 1) {
			SendUploadResponse(session, avatar, ErrorCodes::FileSeqInvalid, md5, name, seq,
				total_size, trans_size, last, uid);
			return;
		}
		file_info->_accepted_seq = seq; //后续分片可以继续进入窗口
	}

	//写盘是异步的：文件线程只回结果，这里补齐请求上下文字段再回包
	auto callback = [session, avatar, md5, name, seq, total_size, trans_size, last, uid]
		(const Json::Value& result) {
		Json::Value response = result; //error 与文件线程产出的字段
		response["md5"] = md5;
		response["name"] = name;
		response["seq"] = seq;
		response["total_size"] = Json::Int64(total_size);
		response["trans_size"] = Json::Int64(trans_size);
		response["last"] = last;
		response["uid"] = uid;
		session->Send(response.toStyledString(),
			avatar ? ID_UPLOAD_HEAD_ICON_RSP : ID_UPLOAD_FILE_RSP);

		//成功收尾才清内存进度；失败保留记录，客户端重试仍可续传
		if (result["error"].asInt() == ErrorCodes::Success && last == 1)
			LogicSystem::GetInstance()->RemoveFileInfo(md5);
	};

	auto task = std::make_shared<FileTask>(
		session, uid, file_path.string(), name, md5, seq, total_size, trans_size, last,
		root["data"].asString(), avatar, std::move(callback));
	task->_chat_msg_id = chat_msg_id;
	task->_touid = touid;
	task->_thread_id = chat_thread_id;
	task->_thread_type = thread_type;
	task->_unique_id = unique_id;
	FileSystem::GetInstance()->PostUploadTask(std::move(task));
}

void resource::HandleSync(std::shared_ptr<CSession> session, const std::string& data)
{
	Json::Value root;
	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Error_Json;

	if (!ParseJson(data, root) || !root["md5"].isString() || !root["uid"].isInt() ||
		!root["token"].isString()) {
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP); //字段不全直接回错误
		return;
	}

	const std::string md5 = root["md5"].asString(); //要查的传输
	const int uid = root["uid"].asInt(); //请求者
	const std::string token = root["token"].asString(); //重新校验身份

	if (!IsMd5(md5)) {
		rtvalue["error"] = ErrorCodes::FileDataInvalid; //续传键格式不对
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	const int auth_error = EnsureSessionUser(session, uid, token);
	if (auth_error != ErrorCodes::Success) {
		rtvalue["error"] = auth_error; //uid没有令牌或与连接身份不符
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	std::shared_ptr<FileInfo> file_info;
	if (!LogicSystem::GetInstance()->LoadFileInfo(md5, file_info)) { //内存/Redis都没有
		rtvalue["error"] = ErrorCodes::FileNotExists;
		rtvalue["md5"] = md5;
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	rtvalue["error"] = ErrorCodes::Success; //把已落盘位置告诉客户端
	rtvalue["md5"] = md5;
	rtvalue["name"] = file_info->_name;
	rtvalue["seq"] = file_info->_seq; //最后落盘分片
	rtvalue["trans_size"] = Json::Int64(file_info->_trans_size); //已落盘字节数
	rtvalue["total_size"] = Json::Int64(file_info->_total_size); //总大小
	session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
}

void resource::HandleDownload(std::shared_ptr<CSession> session, const std::string& data)
{
	Json::Value root;
	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Error_Json;

	if (!ParseJson(data, root) || !root["uid"].isInt() || !root["token"].isString() ||
		!root["owner_uid"].isInt() || !root["name"].isString() || !root["seq"].isInt()) {
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP); //字段不全
		return;
	}

	const int uid = root["uid"].asInt(); //请求者，用于token校验
	const std::string token = root["token"].asString();
	const int owner_uid = root["owner_uid"].asInt(); //文件属于谁
	const std::string name = root["name"].asString(); //资源文件名
	const int seq = root["seq"].asInt(); //要第几片

	if (!IsSafeName(name) || owner_uid <= 0 || seq < 1) {
		rtvalue["error"] = ErrorCodes::FileNameInvalid; //文件名/归属/序号非法
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	const int auth_error = EnsureSessionUser(session, uid, token);
	if (auth_error != ErrorCodes::Success) {
		rtvalue["error"] = auth_error; //未认证、uid无效或换了uid
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	//上传与下载统一按归属用户分目录，owner_uid就是上传时的uid
	auto path = ConfigMgr::Inst().GetStoragePath() / std::to_string(owner_uid) / name;
	auto callback = [session, name, seq](const Json::Value& result) {
		//错误时只有error，成功时带data/is_last/current_size/total_size
		Json::Value response = result;
		response["name"] = name; //客户端按资源名找回本次下载
		response["seq"] = seq;
		session->Send(response.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
	};

	FileSystem::GetInstance()->PostDownloadTask(std::make_shared<DownloadTask>(
		session, path.string(), name, seq, std::move(callback)));
}
