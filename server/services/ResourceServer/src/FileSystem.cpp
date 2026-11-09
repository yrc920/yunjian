#include "FileSystem.h"
#include "const.h"
#include <functional>

FileSystem::FileSystem()
{
	for (int i = 0; i < FILE_WORKER_COUNT; ++i)
		_file_workers.push_back(std::make_shared<FileWorker>());
	for (int i = 0; i < DOWNLOAD_WORKER_COUNT; ++i)
		_download_workers.push_back(std::make_shared<DownloadWorker>());
}

FileSystem::~FileSystem()
{
	Stop();
}

//同一文件名的分片固定落到同一线程，保证追加顺序；不同文件之间并行
void FileSystem::PostUploadTask(std::shared_ptr<FileTask> task)
{
	if (_file_workers.empty())
		return;
	auto index = std::hash<std::string>{}(task->_name) % _file_workers.size();
	_file_workers[index]->PostTask(std::move(task));
}

void FileSystem::PostDownloadTask(std::shared_ptr<DownloadTask> task)
{
	if (_download_workers.empty())
		return;
	auto index = std::hash<std::string>{}(task->_name) % _download_workers.size();
	_download_workers[index]->PostTask(std::move(task));
}

void FileSystem::Stop()
{
	for (auto& worker : _file_workers)
		worker->Stop();
	for (auto& worker : _download_workers)
		worker->Stop();
}
