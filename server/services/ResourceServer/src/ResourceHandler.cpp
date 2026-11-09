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

bool ValidateToken(int uid, const std::string& token)
{
	if (uid <= 0 || token.empty())
		return false;

	std::string value;
	if (!RedisMgr::GetInstance()->Get(USERTOKENPREFIX + std::to_string(uid), value))
		return false;
	return value == token;
}

bool EnsureSessionUser(const std::shared_ptr<CSession>& session, int uid, const std::string& token)
{
	if (uid <= 0)
		return false;

	const int bound_uid = session->GetUserId();
	if (bound_uid != 0)
		return bound_uid == uid;

	if (!ValidateToken(uid, token))
		return false;
	return session->BindUser(uid);
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

	if (!EnsureSessionUser(session, uid, token)) { //首片校验token，后续校验连接绑定
		SendUploadResponse(session, avatar, ErrorCodes::TokenInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	//头像按用户分目录，通用文件单独存放；文件名已在上面排除路径分隔符
	auto storage = ConfigMgr::Inst().GetStoragePath();
	boost::filesystem::path file_path = avatar
		? storage / std::to_string(uid) / name
		: storage / "files" / name;

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

		//客户端必须一片一片等回包，序号跳变说明中间丢片，直接拒绝而不是写出空洞
		if (seq != file_info->_seq + 1) {
			SendUploadResponse(session, avatar, ErrorCodes::FileSeqInvalid, md5, name, seq,
				total_size, trans_size, last, uid);
			return;
		}
	}

	//写盘是异步的：FileWorker完成后再回包，成功且是最后一片时清掉内存进度
	auto callback = [session, avatar, md5, name, seq, total_size, trans_size, last, uid](int error) {
		SendUploadResponse(session, avatar, error, md5, name, seq, total_size, trans_size, last, uid);
		if (error == ErrorCodes::Success && last == 1) {
			LogicSystem::GetInstance()->RemoveFileInfo(md5);
		}
	};

	FileSystem::GetInstance()->PostUploadTask(std::make_shared<FileTask>(
		session, uid, file_path.string(), name, md5, seq, total_size, trans_size, last,
		root["data"].asString(), avatar, std::move(callback)));
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

	if (!IsMd5(md5) || !EnsureSessionUser(session, uid, token)) {
		rtvalue["error"] = ErrorCodes::TokenInvalid; //md5非法或token不匹配
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

	if (!EnsureSessionUser(session, uid, token)) {
		rtvalue["error"] = ErrorCodes::TokenInvalid; //未认证或换了uid
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	//头像目录按owner_uid分：下载好友头像时也能找到对方的文件
	auto path = ConfigMgr::Inst().GetStoragePath() / std::to_string(owner_uid) / name;
	auto callback = [session, name, seq](int error, std::string file_data,
		long long current_size, long long total_size, bool last) {
		//错误时只回error/name/seq，成功才带数据和偏移，客户端据此决定是否继续请求
		Json::Value response;
		response["error"] = error;
		response["name"] = name;
		response["seq"] = seq;
		if (error == ErrorCodes::Success) {
			response["data"] = std::move(file_data); //Base64分片
			response["is_last"] = last; //最后一片
			response["current_size"] = Json::Int64(current_size); //本片结束偏移
			response["total_size"] = Json::Int64(total_size); //文件总大小
		}
		session->Send(response.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
	};

	FileSystem::GetInstance()->PostDownloadTask(std::make_shared<DownloadTask>(
		session, path.string(), name, seq, std::move(callback)));
}
