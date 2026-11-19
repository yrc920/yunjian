#include "ResourceHandler.h"
#include "ConfigMgr.h"
#include "FileSystem.h"
#include "LogicSystem.h"
#include "MysqlDao.h"
#include "RedisMgr.h"
#include "const.h"
#include <boost/filesystem.hpp>
#include <json/json.h>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include "Log.h"

namespace {

//上传与下载的帧体：4 字节头长（网络序）+ JSON 头 + 原始分片。
//分片直接跟在头后面，JSON 头只剩几百字节，两端都不必再做 Base64
bool SplitFrame(const std::string& body, Json::Value& header, const char*& payload,
	std::size_t& payload_len)
{
	if (body.size() < 4)
		return false;
	const auto* bytes = reinterpret_cast<const unsigned char*>(body.data());
	const std::uint32_t header_len = (static_cast<std::uint32_t>(bytes[0]) << 24)
		| (static_cast<std::uint32_t>(bytes[1]) << 16)
		| (static_cast<std::uint32_t>(bytes[2]) << 8)
		| static_cast<std::uint32_t>(bytes[3]);
	//头部是固定字段组成的对象，正常不超过 1 KB；上限拦住畸形帧。
	//先减后比，避免 header_len 接近 UINT32_MAX 时 4 + header_len 溢出绕过检查
	if (header_len == 0 || header_len > 8192 || header_len > body.size() - 4)
		return false;
	Json::Reader reader;
	if (!reader.parse(body.substr(4, header_len), header) || !header.isObject())
		return false;
	payload = body.data() + 4 + header_len;
	payload_len = body.size() - 4 - header_len;
	return true;
}

std::string BuildFrame(const Json::Value& header, const char* payload, std::size_t payload_len)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = ""; //紧凑输出，头部只有几百字节
	const std::string text = Json::writeString(builder, header);

	std::string frame;
	frame.reserve(4 + text.size() + payload_len);
	const auto len = static_cast<std::uint32_t>(text.size());
	frame.push_back(static_cast<char>((len >> 24) & 0xFF));
	frame.push_back(static_cast<char>((len >> 16) & 0xFF));
	frame.push_back(static_cast<char>((len >> 8) & 0xFF));
	frame.push_back(static_cast<char>(len & 0xFF));
	frame += text;
	if (payload_len > 0)
		frame.append(payload, payload_len);
	return frame;
}

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

//用户已用存储：统计该用户目录下的所有文件（含上传中的 .part）。
//只在首片调用一次，文件数上千时也只是一次目录遍历
long long StorageUsage(int uid)
{
	boost::system::error_code ec;
	auto dir = ConfigMgr::Inst().GetStoragePath() / std::to_string(uid);
	if (!boost::filesystem::exists(dir, ec))
		return 0;

	long long total = 0;
	boost::filesystem::recursive_directory_iterator it(dir, ec), end;
	while (it != end && !ec) {
		if (boost::filesystem::is_regular_file(it->path(), ec))
			total += static_cast<long long>(boost::filesystem::file_size(it->path(), ec));
		it.increment(ec);
	}
	return total;
}

//上传前检查配额：0 表示不限制；配置缺失时用默认值
bool CheckQuota(int uid, const std::string& name, long long incoming)
{
	const std::string configured = ConfigMgr::Inst().GetValue("Storage", "QuotaMB");
	const long long quota_mb = configured.empty()
		? DEFAULT_QUOTA_MB : std::atoll(configured.c_str());
	if (quota_mb <= 0)
		return true;

	const long long limit = quota_mb * 1024 * 1024;
	long long used = StorageUsage(uid);

	//同名文件会被覆盖，重传前先把它已占的空间扣掉，否则重发同一张大图会被配额挡住
	boost::system::error_code ec;
	auto dir = ConfigMgr::Inst().GetStoragePath() / std::to_string(uid);
	for (const std::string& candidate : { name, name + UPLOAD_PART_SUFFIX }) {
		const auto path = dir / candidate;
		if (boost::filesystem::is_regular_file(path, ec))
			used -= static_cast<long long>(boost::filesystem::file_size(path, ec));
	}
	if (used < 0)
		used = 0;

	if (used + incoming <= limit)
		return true;

	LOG_WARN << "[resource] quota exceeded uid=" << uid << " used=" << used
		<< " incoming=" << incoming << " limit=" << limit;
	return false;
}

//下载鉴权结果缓存：10 MB 文件会分 160 片，每片都查库代价太高。
//按 (请求者, 归属, 文件名) 缓存 60 秒，成员或头像变更最多滞后一分钟
bool AccessAllowed(int requester, int owner, const std::string& name)
{
	using Clock = std::chrono::steady_clock;
	struct Entry {
		Clock::time_point expires;
		bool allowed;
	};

	static std::mutex mutex;
	static std::unordered_map<std::string, Entry> cache;

	const std::string key = std::to_string(requester) + ":" + std::to_string(owner) + ":" + name;
	const auto now = Clock::now();
	{
		std::lock_guard<std::mutex> lock(mutex);
		auto iter = cache.find(key);
		if (iter != cache.end() && iter->second.expires > now)
			return iter->second.allowed;
	}

	const bool allowed = MysqlDao::GetInstance()->CanAccessResource(requester, owner, name);
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (cache.size() > 4096)
			cache.clear(); //条目按"人+文件"增长，整体清空比逐条淘汰更省事
		cache[key] = Entry{ now + std::chrono::seconds(60), allowed };
	}
	return allowed;
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
	const char* payload = nullptr;
	std::size_t payload_len = 0;
	if (!SplitFrame(data, root, payload, payload_len)) { //帧头不合法
		SendUploadResponse(session, avatar, ErrorCodes::Error_Json, "", "", 0, 0, 0, 0, 0);
		return;
	}

	//类型必须逐项检查：JsonCpp的asInt()会把字符串转成0，不能先取值再判断
	if (!root["md5"].isString() || !root["name"].isString() ||
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
		payload_len == 0 || payload_len > MAX_FILE_LEN ||
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

	//头像与通用文件都按用户分目录：下载时拿 owner_uid 就能拼出同一路径。
	//上传期间先写 .part，最后一片落盘后由文件线程改名，中断的上传不会变成正式资源
	auto storage = ConfigMgr::Inst().GetStoragePath();
	boost::filesystem::path file_path =
		storage / std::to_string(uid) / (name + UPLOAD_PART_SUFFIX);

	//配额只在首片检查：按用户目录已用空间判断，避免传完才发现放不下
	if (seq == 1 && !CheckQuota(uid, name, total_size)) {
		SendUploadResponse(session, avatar, ErrorCodes::FileQuotaExceeded, md5, name, seq,
			total_size, trans_size, last, uid);
		return;
	}

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
			//末片回包丢失后客户端会重发：文件已按同名同大小就位时按成功返回，
			//不让一次丢包把整个上传判成失败（完成时进度记录已删除，这里必须兜底）
			boost::system::error_code ec;
			const auto final_path = storage / std::to_string(uid) / name;
			const bool already_done = last == 1 && trans_size == total_size
				&& boost::filesystem::exists(final_path, ec)
				&& boost::filesystem::file_size(final_path, ec)
					== static_cast<boost::uintmax_t>(total_size);
			if (already_done) {
				SendUploadResponse(session, avatar, ErrorCodes::Success, md5, name, seq,
					total_size, trans_size, last, uid);
				return;
			}

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

		//成功收尾才清进度；失败保留记录，客户端重试仍可续传
		if (result["error"].asInt() == ErrorCodes::Success && last == 1) {
			LogicSystem::GetInstance()->RemoveFileInfo(md5);
			//传输已结束，续传记录不再需要；留着只会让后续同名上传误判为"可续传"
			RedisMgr::GetInstance()->Del(FILE_INFO_PREFIX + md5);
		}
	};

	auto task = std::make_shared<FileTask>(
		session, uid, file_path.string(), name, md5, seq, total_size, trans_size, last,
		std::string(payload, payload_len), avatar, std::move(callback));
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

namespace {

//下载回包统一走二进制帧：错误也只有 JSON 头、没有分片。
//name 与 seq 能确定时一并回传，客户端据此找回本次下载并显示对应错误
void SendDownloadError(const std::shared_ptr<CSession>& session, int error,
	const std::string& name = std::string(), int seq = 0)
{
	Json::Value header;
	header["error"] = error;
	if (!name.empty())
		header["name"] = name;
	if (seq > 0)
		header["seq"] = seq;
	session->Send(BuildFrame(header, nullptr, 0), ID_DOWN_LOAD_FILE_RSP);
}

} // namespace

void resource::HandleDownload(std::shared_ptr<CSession> session, const std::string& data)
{
	Json::Value root;
	if (!ParseJson(data, root) || !root["uid"].isInt() || !root["token"].isString() ||
		!root["owner_uid"].isInt() || !root["name"].isString() || !root["seq"].isInt()) {
		SendDownloadError(session, ErrorCodes::Error_Json); //字段不全，连名字都没有
		return;
	}

	const int uid = root["uid"].asInt(); //请求者，用于token校验
	const std::string token = root["token"].asString();
	const int owner_uid = root["owner_uid"].asInt(); //文件属于谁
	const std::string name = root["name"].asString(); //资源文件名
	const int seq = root["seq"].asInt(); //要第几片

	if (!IsSafeName(name) || owner_uid <= 0 || seq < 1) {
		SendDownloadError(session, ErrorCodes::FileNameInvalid, name, seq); //文件名/归属/序号非法
		return;
	}

	const int auth_error = EnsureSessionUser(session, uid, token);
	if (auth_error != ErrorCodes::Success) {
		SendDownloadError(session, auth_error, name, seq); //未认证、uid无效或换了uid
		return;
	}

	//只有本人、共用会话的成员，或对方当前头像可以下载
	if (!AccessAllowed(uid, owner_uid, name)) {
		LOG_WARN << "[resource] download denied requester=" << uid
			<< " owner=" << owner_uid << " name=" << name;
		//对外与"文件不存在"一致，不向非成员暴露该文件是否存在
		SendDownloadError(session, ErrorCodes::FileNotExists, name, seq);
		return;
	}

	//上传与下载统一按归属用户分目录，owner_uid就是上传时的uid
	auto path = ConfigMgr::Inst().GetStoragePath() / std::to_string(owner_uid) / name;
	auto callback = [session, name, seq](const Json::Value& result, const std::string& payload) {
		//错误时只有error，成功时带 is_last / current_size / total_size 与原始分片
		Json::Value response = result;
		response["name"] = name; //客户端按资源名找回本次下载
		response["seq"] = seq;
		session->Send(BuildFrame(response, payload.data(), payload.size()),
			ID_DOWN_LOAD_FILE_RSP);
	};

	FileSystem::GetInstance()->PostDownloadTask(std::make_shared<DownloadTask>(
		session, path.string(), name, seq, std::move(callback)));
}
