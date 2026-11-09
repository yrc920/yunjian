#include "FileSystem.h"
#include "const.h"
#include <functional>

FileSystem::FileSystem()
{
	//上传与下载分开建池：下载只读，和落盘互不阻塞
	for (int i = 0; i < FILE_WORKER_COUNT; ++i)
		_file_workers.push_back(std::make_shared<FileWorker>()); //落盘线程
	for (int i = 0; i < DOWNLOAD_WORKER_COUNT; ++i)
		_download_workers.push_back(std::make_shared<DownloadWorker>()); //只读下载线程
}

FileSystem::~FileSystem()
{
	Stop(); //析构时join所有线程
}

void FileSystem::PostUploadTask(std::shared_ptr<FileTask> task)
{
	if (_file_workers.empty())
		return; //理论上不会发生，防御空池
	auto index = std::hash<std::string>{}(task->_name) % _file_workers.size(); //同名固定同一线程
	_file_workers[index]->PostTask(std::move(task)); //移动所有权入队
}

void FileSystem::PostDownloadTask(std::shared_ptr<DownloadTask> task)
{
	if (_download_workers.empty())
		return;
	auto index = std::hash<std::string>{}(task->_name) % _download_workers.size(); //同一文件顺序读
	_download_workers[index]->PostTask(std::move(task));
}

void FileSystem::Stop()
{
	for (auto& worker : _file_workers)
		worker->Stop(); //先停上传
	for (auto& worker : _download_workers)
		worker->Stop(); //再停下载
}
