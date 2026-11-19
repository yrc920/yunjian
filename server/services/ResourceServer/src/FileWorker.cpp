#include "FileWorker.h"
#include "ChatGrpcClient.h"
#include "LogicSystem.h"
#include "MysqlDao.h"
#include "RedisMgr.h"
#include "const.h"
#include <boost/filesystem.hpp>
#include <json/json.h>
#include <fstream>
#include <iostream>
#include "Log.h"

namespace {

//文件线程只把结果放进Json，回包字段由逻辑层的回调补齐
void FinishTask(const std::shared_ptr<FileTask>& task, int error)
{
	//写盘失败时把“已接收序号”退回落盘位置，客户端重传这一片不会因为窗口超前面被拒
	if (error != ErrorCodes::Success) {
		auto info = LogicSystem::GetInstance()->GetFileInfo(task->_md5);
		if (info)
			info->_accepted_seq = info->_seq;
	}
	Json::Value result;
	result["error"] = error;
	if (task->_callback)
		task->_callback(result);
}

}

FileWorker::FileWorker()
{
	_work_thread = std::thread([this]() { //每个worker独占一个线程
		while (!_b_stop.load()) { //停服标志由Stop置位
			std::unique_lock<std::mutex> lock(_mtx); //保护任务队列
			_cv.wait(lock, [this] {
				return _b_stop.load() || !_task_que.empty(); //有任务或要停服才醒
			});

			if (_b_stop.load()) //停服优先，未处理的任务直接丢弃
				break;

			//取出任务后立刻解锁：写盘与数据库更新都不需要占着队列锁
			auto task = _task_que.front(); //先取队首
			_task_que.pop(); //再弹出，避免拷贝
			lock.unlock(); //后面的IO与数据库操作不占队列锁

			TaskCallback(task); //在本线程完成解码、写盘、回包
		}
	});
}

FileWorker::~FileWorker()
{
	Stop(); //析构时保证线程已join
}

void FileWorker::PostTask(std::shared_ptr<FileTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx); //入队需要锁
		if (_b_stop.load()) //停服后不再接收新任务
			return;
		_task_que.push(std::move(task)); //移动所有权，避免引用计数开销
	}
	_cv.notify_one(); //唤醒一个等待中的worker
}

void FileWorker::Stop()
{
	if (_b_stop.exchange(true)) //已经停过，重复调用直接返回
		return;
	_cv.notify_all(); //唤醒所有等待线程
	if (_work_thread.joinable()) //等待当前任务处理完
		_work_thread.join();
}

void FileWorker::TaskCallback(std::shared_ptr<FileTask> task)
{
	//偏移必须与seq对应：分片为空、超长或与本片声明的 trans_size 对不上都拒收
	const std::string& data = task->_data;
	const long long expected_trans = static_cast<long long>(task->_seq - 1) * MAX_FILE_LEN
		+ static_cast<long long>(data.size());
	if (task->_seq < 1 || data.empty() || data.size() > MAX_FILE_LEN
		|| expected_trans != task->_trans_size) {
		FinishTask(task, ErrorCodes::FileDataInvalid); //序号、长度、偏移任一项不符
		return;
	}

	try {
		//头像目录按uid分层，第一次上传时目录通常还不存在
		boost::filesystem::path file_path(task->_path); //目标文件路径
		auto dir_path = file_path.parent_path(); //父目录
		if (!dir_path.empty() && !boost::filesystem::exists(dir_path)) {
			boost::filesystem::create_directories(dir_path); //首次上传时补建目录
		}

		std::ios::openmode mode = std::ios::binary; //文件按二进制写，避免换行转换
		mode |= (task->_seq == 1) ? std::ios::trunc : std::ios::app; //首片覆盖，其余追加
		std::ofstream outfile(task->_path, mode); //打开目标文件
		if (!outfile) { //目录权限或磁盘问题
			LOG_ERROR << "无法打开文件进行写入: " << task->_path;
			FinishTask(task, ErrorCodes::FileWritePermissionFailed);
			return;
		}

		outfile.write(data.data(), static_cast<std::streamsize>(data.size())); //写本片
		if (!outfile) { //写满磁盘或IO错误
			LOG_ERROR << "写入文件失败: " << task->_path;
			FinishTask(task, ErrorCodes::FileWritePermissionFailed);
			return;
		}
		outfile.close(); //关闭后数据才真正落盘
	}
	catch (std::exception& e) { //文件系统异常不能逃出线程
		LOG_ERROR << "FileWorker exception: " << e.what();
		FinishTask(task, ErrorCodes::FileWritePermissionFailed);
		return;
	}

	//最后一片落盘后去掉 .part：正式资源名只会指向完整文件，
	//中断的上传留在后缀里，由清理线程按时间回收
	if (task->_last == 1) {
		boost::system::error_code ec;
		const boost::filesystem::path final_path =
			boost::filesystem::path(task->_path).parent_path() / task->_name;
		if (boost::filesystem::exists(final_path, ec))
			boost::filesystem::remove(final_path, ec); //同名重传覆盖旧文件
		boost::filesystem::rename(task->_path, final_path, ec);
		if (ec) {
			LOG_ERROR << "rename upload failed: " << task->_path << " -> "
				<< final_path.string() << " (" << ec.message() << ")";
			FinishTask(task, ErrorCodes::FileWritePermissionFailed);
			return;
		}
	}

	//写盘成功后再推进进度，同步接口返回的就是已经落盘的字节数；
	//窗口发送可能让多个分片同时在途，进度只能向前，不能被较早的分片回退。
	//Redis 只在每 16 片或每 1 MB 落一次，最后一片必落：2KB 分片下每片写一次 Redis
	//会让 10MB 文件产生 5120 次 SET，远比文件本体传输贵
	auto file_info = LogicSystem::GetInstance()->GetFileInfo(task->_md5);
	if (file_info && task->_seq > file_info->_seq) {
		const long long previous = file_info->_trans_size;
		file_info->_seq = task->_seq; //最后落盘的分片序号
		file_info->_trans_size = task->_trans_size; //已落盘字节数
		const bool need_save = task->_last || (task->_seq % 16 == 0)
			|| (file_info->_trans_size - previous >= 1024 * 1024);
		if (need_save)
			LogicSystem::GetInstance()->SaveFileInfo(file_info); //同步到Redis
	}

	if (task->_last && task->_avatar) { //最后一片且是头像
		//只有头像需要写用户表；通用文件保持原样，等后续资源属性表接入
		UpdateAvatarInfo(task->_uid, task->_name);
	}
	else if (task->_last && task->_chat_msg_id > 0) { //聊天图片全部落盘
		//先尝试通知接收方所在的 ChatServer：它会回写消息状态并推送下载通知；
		//接收方离线或 RPC 不可用时直接写库，保证下次登录仍能看到“已上传”
		const bool notified = ChatGrpcClient::GetInstance()->NotifyImageReady(
			task->_chat_msg_id, task->_uid, task->_touid, task->_thread_id, task->_name,
			task->_unique_id, task->_total_size, task->_thread_type);
		if (!notified) {
			const bool updated = MysqlDao::GetInstance()->UpdateChatMessageStatus(
				task->_chat_msg_id, task->_uid, 1);
			if (!updated)
				LOG_ERROR << "update chat message status failed, id: " << task->_chat_msg_id;
		}
	}

	FinishTask(task, ErrorCodes::Success); //通知逻辑线程回包
}

void FileWorker::UpdateAvatarInfo(int uid, const std::string& name)
{
	if (!MysqlDao::GetInstance()->UpdateUserIcon(uid, name)) { //先写user.icon
		LOG_ERROR << "update user icon failed, uid: " << uid;
		return;
	}

	auto user_info = MysqlDao::GetInstance()->GetUser(uid); //取完整资料刷新缓存
	if (!user_info) {
		return;
	}

	Json::Value base_root;
	base_root["uid"] = user_info->uid; //缓存字段与ChatServer登录回包一致
	base_root["name"] = user_info->name;
	base_root["email"] = user_info->email;
	base_root["nick"] = user_info->nick;
	base_root["desc"] = user_info->desc;
	base_root["sex"] = user_info->sex;
	base_root["icon"] = user_info->icon; //新头像文件名
	RedisMgr::GetInstance()->Set(USER_BASE_INFO + std::to_string(uid),
		base_root.toStyledString()); //按uid缓存

	Json::Value name_root = base_root; //用户名索引缓存内容相同
	RedisMgr::GetInstance()->Set(NAME_INFO + user_info->name, name_root.toStyledString());

	//头像文件名变了, 让 ChatServer 把新资料推给在线好友; 失败只记日志,
	//好友下次登录仍会从 1006 回包里拿到新的 icon 文件名
	if (!ChatGrpcClient::GetInstance()->BroadcastProfileChanged(uid)) {
		LOG_WARN << "broadcast avatar change failed, uid: " << uid;
	}
}
