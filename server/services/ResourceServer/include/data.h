#pragma once
// 资源服务器使用的数据结构
//

#include <string>
#include <memory>
#include <mutex>

struct UserInfo {
	UserInfo() : name(""), pwd(""), uid(0), email(""), nick(""),
		desc(""), sex(0), icon("") {}

	std::string name;
	std::string pwd;
	int uid;
	std::string email;
	std::string nick;
	std::string desc;
	int sex;
	std::string icon;
};

//一次上传的服务器端进度，内存与Redis保存同一份内容
struct FileInfo {
	std::string _md5;
	std::string _name;
	std::string _file_path;
	int _uid = 0;
	int _seq = 0; //已落盘的最后一片
	int _accepted_seq = 0; //逻辑线程已接收的序号；窗口发送时可比落盘进度超前
	long long _total_size = 0;
	long long _trans_size = 0;
	bool _avatar = false;
};
