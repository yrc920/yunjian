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

//只接受纯文件名，避免../或绝对路径把文件写到存储目录之外
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

//首个请求用token绑定uid；后续分片只做连接内一致性检查，不再频繁查Redis
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
	if (!ParseJson(data, root)) {
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}

	if (!root["md5"].isString() || !root["name"].isString() || !root["data"].isString() ||
		!root["seq"].isInt() || !root["trans_size"].isInt64() || !root["total_size"].isInt64() ||
		!root["last"].isInt() || !root["uid"].isInt() || !root["token"].isString()) {
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}

	const std::string md5 = root["md5"].asString();
	const std::string name = root["name"].asString();
	const std::string token = root["token"].asString();
	const int uid = root["uid"].asInt();
	const int seq = root["seq"].asInt();
	const long long total_size = root["total_size"].asInt64();
	const long long trans_size = root["trans_size"].asInt64();
	const int last = root["last"].asInt();

	if (!IsMd5(md5) || !IsSafeName(name)) {
		SendUploadResponse(session, avatar, ErrorCodes::FileNameInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	if (total_size <= 0 || total_size > MAX_FILE_SIZE) {
		SendUploadResponse(session, avatar, ErrorCodes::FileTooLarge, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	if (seq < 1 || trans_size <= 0 || trans_size > total_size ||
		(last == 1) != (trans_size == total_size)) {
		SendUploadResponse(session, avatar, ErrorCodes::FileDataInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	if (!EnsureSessionUser(session, uid, token)) {
		SendUploadResponse(session, avatar, ErrorCodes::TokenInvalid, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

	//头像按用户分目录，通用文件单独存放；文件名已在上面排除路径分隔符
	auto storage = ConfigMgr::Inst().GetStoragePath();
	boost::filesystem::path file_path = avatar
		? storage / std::to_string(uid) / name
		: storage / "files" / name;

	std::shared_ptr<FileInfo> file_info;
	if (seq == 1) {
		file_info = std::make_shared<FileInfo>();
		file_info->_md5 = md5;
		file_info->_name = name;
		file_info->_file_path = file_path.string();
		file_info->_uid = uid;
		file_info->_total_size = total_size;
		file_info->_avatar = avatar;
		LogicSystem::GetInstance()->AddFileInfo(file_info);
	}
	else {
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

		if (seq != file_info->_seq + 1) {
			SendUploadResponse(session, avatar, ErrorCodes::FileSeqInvalid, md5, name, seq,
				total_size, trans_size, last, uid);
			return;
		}
	}

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
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	const std::string md5 = root["md5"].asString();
	const int uid = root["uid"].asInt();
	const std::string token = root["token"].asString();

	if (!IsMd5(md5) || !EnsureSessionUser(session, uid, token)) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	std::shared_ptr<FileInfo> file_info;
	if (!LogicSystem::GetInstance()->LoadFileInfo(md5, file_info)) {
		rtvalue["error"] = ErrorCodes::FileNotExists;
		rtvalue["md5"] = md5;
		session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
		return;
	}

	rtvalue["error"] = ErrorCodes::Success;
	rtvalue["md5"] = md5;
	rtvalue["name"] = file_info->_name;
	rtvalue["seq"] = file_info->_seq;
	rtvalue["trans_size"] = Json::Int64(file_info->_trans_size);
	rtvalue["total_size"] = Json::Int64(file_info->_total_size);
	session->Send(rtvalue.toStyledString(), ID_SYNC_FILE_RSP);
}

void resource::HandleDownload(std::shared_ptr<CSession> session, const std::string& data)
{
	Json::Value root;
	Json::Value rtvalue;
	rtvalue["error"] = ErrorCodes::Error_Json;

	if (!ParseJson(data, root) || !root["uid"].isInt() || !root["token"].isString() ||
		!root["owner_uid"].isInt() || !root["name"].isString() || !root["seq"].isInt()) {
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	const int uid = root["uid"].asInt();
	const std::string token = root["token"].asString();
	const int owner_uid = root["owner_uid"].asInt();
	const std::string name = root["name"].asString();
	const int seq = root["seq"].asInt();

	if (!IsSafeName(name) || owner_uid <= 0 || seq < 1) {
		rtvalue["error"] = ErrorCodes::FileNameInvalid;
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	if (!EnsureSessionUser(session, uid, token)) {
		rtvalue["error"] = ErrorCodes::TokenInvalid;
		session->Send(rtvalue.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
		return;
	}

	auto path = ConfigMgr::Inst().GetStoragePath() / std::to_string(owner_uid) / name;
	auto callback = [session, name, seq](int error, std::string file_data,
		long long current_size, long long total_size, bool last) {
		Json::Value response;
		response["error"] = error;
		response["name"] = name;
		response["seq"] = seq;
		if (error == ErrorCodes::Success) {
			response["data"] = std::move(file_data);
			response["is_last"] = last;
			response["current_size"] = Json::Int64(current_size);
			response["total_size"] = Json::Int64(total_size);
		}
		session->Send(response.toStyledString(), ID_DOWN_LOAD_FILE_RSP);
	};

	FileSystem::GetInstance()->PostDownloadTask(std::make_shared<DownloadTask>(
		session, path.string(), name, seq, std::move(callback)));
}
