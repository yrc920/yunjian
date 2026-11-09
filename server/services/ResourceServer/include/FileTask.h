#pragma once
// 文件落盘与下载任务
//

#include <memory>
#include <string>
#include <functional>
#include <cstdint>

class CSession;

struct FileTask {
	FileTask(std::shared_ptr<CSession> session, int uid, std::string path, std::string name,
		std::string md5, int seq, long long total_size, long long trans_size, int last,
		std::string data, bool avatar, std::function<void(int)> callback)
		: _session(std::move(session)), _uid(uid), _path(std::move(path)), _name(std::move(name)),
		_md5(std::move(md5)), _seq(seq), _total_size(total_size), _trans_size(trans_size),
		_last(last), _data(std::move(data)), _avatar(avatar), _callback(std::move(callback)) {}

	std::shared_ptr<CSession> _session;
	int _uid;
	std::string _path;
	std::string _name;
	std::string _md5;
	int _seq;
	long long _total_size;
	long long _trans_size;
	int _last;
	std::string _data; //Base64，解码和写盘都在文件线程完成
	bool _avatar;
	std::function<void(int)> _callback;
};

struct DownloadTask {
	using Callback = std::function<void(int, std::string, long long, long long, bool)>;

	DownloadTask(std::shared_ptr<CSession> session, std::string path, std::string name,
		int seq, Callback callback)
		: _session(std::move(session)), _path(std::move(path)), _name(std::move(name)),
		_seq(seq), _callback(std::move(callback)) {}

	std::shared_ptr<CSession> _session;
	std::string _path;
	std::string _name;
	int _seq;
	Callback _callback;
};
