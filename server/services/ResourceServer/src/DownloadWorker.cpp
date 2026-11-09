#include "DownloadWorker.h"
#include "Base64.h"
#include "const.h"
#include <boost/filesystem.hpp>
#include <fstream>
#include <vector>
#include <iostream>
#include <algorithm>

DownloadWorker::DownloadWorker()
{
	_work_thread = std::thread([this]() { //下载线程与上传线程分开，只读文件
		while (!_b_stop.load()) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this] {
				return _b_stop.load() || !_task_que.empty(); //有任务或停服才醒
			});

			if (_b_stop.load())
				break;

			auto task = _task_que.front(); //取下载请求
			_task_que.pop();
			lock.unlock(); //读文件不占队列锁

			TaskCallback(task); //按seq读一片
		}
	});
}

DownloadWorker::~DownloadWorker()
{
	Stop();
}

void DownloadWorker::PostTask(std::shared_ptr<DownloadTask> task)
{
	{
		std::lock_guard<std::mutex> lock(_mtx);
		if (_b_stop.load())
			return; //停服后不再接收
		_task_que.push(std::move(task)); //转移所有权
	}
	_cv.notify_one(); //唤醒下载线程
}

void DownloadWorker::Stop()
{
	if (_b_stop.exchange(true))
		return; //已经停过
	_cv.notify_all();
	if (_work_thread.joinable())
		_work_thread.join(); //等当前文件读完
}

void DownloadWorker::TaskCallback(std::shared_ptr<DownloadTask> task)
{
	boost::system::error_code ec;
	const auto total_size = static_cast<long long>(boost::filesystem::file_size(task->_path, ec));
	if (ec) { //文件不存在或无权访问
		task->_callback(ErrorCodes::FileNotExists, {}, 0, 0, false);
		return;
	}

	//seq从1开始；offset已经到文件尾说明客户端请求越界
	const long long offset = static_cast<long long>(task->_seq - 1) * MAX_FILE_LEN;
	if (task->_seq < 1 || offset >= total_size) {
		task->_callback(ErrorCodes::FileDataInvalid, {}, 0, total_size, false); //返回真实大小便于排查
		return;
	}

	//最后一片可能不足2048字节，读取长度取剩余字节数
	const long long remain = total_size - offset;
	const std::size_t read_len = static_cast<std::size_t>(
		std::min<long long>(MAX_FILE_LEN, remain));

	std::ifstream infile(task->_path, std::ios::binary); //只读打开，不改变文件
	if (!infile) {
		task->_callback(ErrorCodes::FileNotExists, {}, 0, total_size, false);
		return;
	}

	infile.seekg(offset, std::ios::beg); //跳到本片起始位置
	std::string data(read_len, '\0'); //预分配读取缓冲
	infile.read(data.data(), static_cast<std::streamsize>(read_len));
	if (!infile) { //文件被截断或IO错误
		task->_callback(ErrorCodes::FileWritePermissionFailed, {}, 0, total_size, false);
		return;
	}

	const bool last = offset + static_cast<long long>(read_len) >= total_size; //是否读到文件尾
	task->_callback(ErrorCodes::Success, base64::Encode(data),
		offset + static_cast<long long>(read_len), total_size, last); //数据与偏移一起回给逻辑线程
}
