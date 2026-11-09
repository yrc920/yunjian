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
	_work_thread = std::thread([this]() {
		while (!_b_stop.load()) {
			std::unique_lock<std::mutex> lock(_mtx);
			_cv.wait(lock, [this] {
				return _b_stop.load() || !_task_que.empty();
			});

			if (_b_stop.load())
				break;

			auto task = _task_que.front();
			_task_que.pop();
			lock.unlock();

			TaskCallback(task);
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
			return;
		_task_que.push(std::move(task));
	}
	_cv.notify_one();
}

void DownloadWorker::Stop()
{
	if (_b_stop.exchange(true))
		return;
	_cv.notify_all();
	if (_work_thread.joinable())
		_work_thread.join();
}

void DownloadWorker::TaskCallback(std::shared_ptr<DownloadTask> task)
{
	boost::system::error_code ec;
	const auto total_size = static_cast<long long>(boost::filesystem::file_size(task->_path, ec));
	if (ec) {
		task->_callback(ErrorCodes::FileNotExists, {}, 0, 0, false);
		return;
	}

	const long long offset = static_cast<long long>(task->_seq - 1) * MAX_FILE_LEN;
	if (task->_seq < 1 || offset >= total_size) {
		task->_callback(ErrorCodes::FileDataInvalid, {}, 0, total_size, false);
		return;
	}

	const long long remain = total_size - offset;
	const std::size_t read_len = static_cast<std::size_t>(
		std::min<long long>(MAX_FILE_LEN, remain));

	std::ifstream infile(task->_path, std::ios::binary);
	if (!infile) {
		task->_callback(ErrorCodes::FileNotExists, {}, 0, total_size, false);
		return;
	}

	infile.seekg(offset, std::ios::beg);
	std::string data(read_len, '\0');
	infile.read(data.data(), static_cast<std::streamsize>(read_len));
	if (!infile) {
		task->_callback(ErrorCodes::FileWritePermissionFailed, {}, 0, total_size, false);
		return;
	}

	const bool last = offset + static_cast<long long>(read_len) >= total_size;
	task->_callback(ErrorCodes::Success, base64::Encode(data),
		offset + static_cast<long long>(read_len), total_size, last);
}
