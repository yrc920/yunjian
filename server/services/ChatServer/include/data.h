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
	long long _id = 0; //friend_apply 自增ID, 申请列表按它倒序
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

//会话类型：chat_thread.thread_type 的取值
enum class ThreadType : int {
	Private = 1,
	Group = 2,
};

//群成员角色：数字越小权限越低，判断时统一用"大于等于"比较
enum class GroupRole : int {
	Member = 0,
	Admin = 1,
	Owner = 2,
};

//会话列表条目: 单聊用 peer_* 描述对方，群聊用 group_* 描述群，
//两类会话共用 thread_id 游标，登录时按同一份列表下发
struct ChatThreadInfo {
	long long thread_id = 0; //会话ID, 由 chat_thread 生成
	int thread_type = static_cast<int>(ThreadType::Private); //1 单聊 / 2 群聊
	int peer_uid = 0; //单聊对方用户ID
	std::string peer_name; //对方用户名
	std::string peer_nick; //对方昵称
	std::string peer_icon; //对方头像
	int peer_sex = 0; //对方性别
	std::string group_name; //群名称
	int group_owner = 0; //群主 uid
	int group_member_count = 0; //群成员数, 会话行与群资料页都要显示
	int my_role = 0; //我在这个群里的角色, 单聊为 0
	bool muted = false; //我是否对这个群设置了免打扰
	bool at_me = false; //未读消息里是否有 @我
	//会话最后活动时间(UNIX 秒), 客户端登录首屏排序用; 打开会话拉历史后以消息时间为准
	long long updated_at = 0;
	int unread = 0; //该会话里发给自己的未读消息条数, 登录时一起下发
};

//群资料与"我"在这个群的设置，用于群资料页与会话标题
struct GroupInfo {
	long long thread_id = 0;
	std::string name;
	std::string notice;
	int owner_id = 0;
	long long created_at = 0;
	int member_count = 0;
	int my_role = 0;
	bool muted = false;
	std::string my_nick; //我在本群的昵称，空表示用账号昵称
	long long last_read_message_id = 0;
};

//群成员：name/nick/icon 来自 user 表，group_nick 是成员自己在这个群里的昵称
struct GroupMemberInfo {
	int uid = 0;
	std::string name;
	std::string nick;
	std::string icon;
	int sex = 0;
	int role = static_cast<int>(GroupRole::Member);
	std::string group_nick;
	long long joined_at = 0;
};

//一条聊天消息: message_id 来自 chat_message 自增主键, 决定会话内的显示顺序
struct ChatMessageInfo {
	long long message_id = 0; //全局自增的消息ID, 0 表示尚未落库
	long long thread_id = 0; //所属会话ID
	int sender_id = 0; //发送者用户ID
	int recv_id = 0; //接收者用户ID; 群消息为 0
	std::string client_msg_id; //客户端生成的去重ID, 重发同一条时保持不变
	std::string content; //消息正文; 图片消息这里存资源文件名
	int status = 0; //消息状态: 图片落库时为0(资源未上传), 文本不使用
	int msg_type = 0; //消息类型: 0 文本, 1 图片, 2 视频, 3 文件, 4 群系统消息
	//落库时间, UNIX 时间戳(秒)。用秒而不是格式化字符串, 客户端按本地时区分层显示
	long long created_at = 0;
	bool is_read = false; //接收方是否已读，只对 recv_id 有意义
	bool recalled = false; //是否已撤回，撤回后正文清空、双方都只看到提示
	std::string mentions; //被 @ 的 uid 列表，逗号分隔，如 "1002,1003"
};
