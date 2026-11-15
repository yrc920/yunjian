#pragma once
// 数据结构定义, 包含用户信息和好友申请信息的结构体
//

#include <string>
#include <vector>

//用户信息结构体
struct UserInfo {
	UserInfo() : name(""), pwd(""), uid(0), email(""), nick(""),
		desc(""), sex(0), icon(""), back("") {}
	std::string name; //用户名
	std::string pwd; //密码
	int uid; //用户ID
	std::string email; //邮箱
	std::string nick; //昵称
	std::string desc; //描述
	int sex; //性别
	std::string icon; //头像
	std::string back; //备注
	std::vector<std::string> tags; //好友标签, 只在好友列表里有值
};

//好友申请信息结构体
struct ApplyInfo {
	ApplyInfo(int uid, std::string name, std::string desc,
		std::string icon, std::string nick, int sex, int status)
		:_uid(uid), _name(name), _desc(desc),
		_icon(icon), _nick(nick), _sex(sex), _status(status) {}

	int _uid; //用户ID
	std::string _name; //用户名
	std::string _desc; //描述
	std::string _icon; //头像
	std::string _nick; //昵称
	int _sex; //性别
	int _status; //申请状态，0表示未处理，1表示同意，2表示拒绝
};

//会话列表条目: 本端视角下的一条单聊, peer_* 是会话对面那个人的资料
struct ChatThreadInfo {
	long long thread_id = 0; //会话ID, 由 chat_thread 生成
	int peer_uid = 0; //对方用户ID
	std::string peer_name; //对方用户名
	std::string peer_nick; //对方昵称
	std::string peer_icon; //对方头像
	int peer_sex = 0; //对方性别
	//会话最后活动时间(UNIX 秒), 客户端登录首屏排序用; 打开会话拉历史后以消息时间为准
	long long updated_at = 0;
	int unread = 0; //该会话里发给自己的未读消息条数, 登录时一起下发
};

//一条聊天消息: message_id 来自 chat_message 自增主键, 决定会话内的显示顺序
struct ChatMessageInfo {
	long long message_id = 0; //全局自增的消息ID, 0 表示尚未落库
	long long thread_id = 0; //所属会话ID
	int sender_id = 0; //发送者用户ID
	int recv_id = 0; //接收者用户ID
	std::string client_msg_id; //客户端生成的去重ID, 重发同一条时保持不变
	std::string content; //消息正文; 图片消息这里存资源文件名
	int status = 0; //消息状态: 图片落库时为0(资源未上传), 文本不使用
	int msg_type = 0; //消息类型: 0 文本, 1 图片, 2 视频, 3 文件
	//落库时间, UNIX 时间戳(秒)。用秒而不是格式化字符串, 客户端按本地时区分层显示
	long long created_at = 0;
	bool is_read = false; //接收方是否已读，只对 recv_id 有意义
	bool recalled = false; //是否已撤回，撤回后正文清空、双方都只看到提示
};
