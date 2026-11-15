#pragma once
// 文件落盘与下载任务
//

#include <memory>
#include <string>
#include <functional>
#include <cstdint>
#include <json/value.h>

class CSession;

//异步结果回调：std::function用类型擦除抹掉逻辑层lambda的具体类型，
//文件线程只往result里填error和自己产出的字段，回包字段与业务状态由逻辑层补齐
using ResultCallback = std::function<void(const Json::Value&)>;

struct FileTask {
	FileTask(std::shared_ptr<CSession> session, int uid, std::string path, std::string name,
		std::string md5, int seq, long long total_size, long long trans_size, int last,
		std::string data, bool avatar, ResultCallback callback)
		: _session(std::move(session)), _uid(uid), _path(std::move(path)), _name(std::move(name)),
		_md5(std::move(md5)), _seq(seq), _total_size(total_size), _trans_size(trans_size),
		_last(last), _data(std::move(data)), _avatar(avatar), _callback(std::move(callback)) {}

	std::shared_ptr<CSession> _session; //回包时保持会话存活
	int _uid;
	std::string _path; //已拼好的绝对/相对存储路径
	std::string _name;
	std::string _md5;
	int _seq;
	long long _total_size;
	long long _trans_size; //本片写完后应达到的偏移
	int _last;
	std::string _data; //Base64，解码和写盘都在文件线程完成
	bool _avatar; //最后一片是否要写user.icon
	//聊天图片的消息信息；头像与普通文件为0，最后一片写完后用它回写消息状态
	long long _chat_msg_id = 0;
	int _touid = 0;
	long long _thread_id = 0;
	int _thread_type = 1; //1 单聊 / 2 群聊：群聊完成通知要发给发送者所在实例再广播
	std::string _unique_id;
	ResultCallback _callback; //写盘结果，error由文件线程填
};

struct DownloadTask {
	DownloadTask(std::shared_ptr<CSession> session, std::string path, std::string name,
		int seq, ResultCallback callback)
		: _session(std::move(session)), _path(std::move(path)), _name(std::move(name)),
		_seq(seq), _callback(std::move(callback)) {}

	std::shared_ptr<CSession> _session;
	std::string _path;
	std::string _name;
	int _seq;
	ResultCallback _callback; //error、Base64数据、当前偏移、总大小、是否最后一片
};
