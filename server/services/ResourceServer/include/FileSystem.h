#pragma once
// 文件线程池：上传按文件名分派落盘，下载按文件名分派读取
//

#include "Singleton.h"
#include "FileTask.h"
#include "FileWorker.h"
#include "DownloadWorker.h"
#include <vector>
#include <memory>

class FileSystem : public Singleton<FileSystem>
{
	friend Singleton<FileSystem>;
public:
	~FileSystem();

	void PostUploadTask(std::shared_ptr<FileTask> task);
	void PostDownloadTask(std::shared_ptr<DownloadTask> task);
	void Stop();

private:
	FileSystem();

	std::vector<std::shared_ptr<FileWorker>> _file_workers;
	std::vector<std::shared_ptr<DownloadWorker>> _download_workers;
};
