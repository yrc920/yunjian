#pragma once
// 常量定义和工具类
//

#include <functional>
#include <string>

enum ErrorCodes {
	Success = 0,
	Error_Json = 1001, //Json解析错误
	RPCFailed = 1002, //RPC请求错误
	VerifyExpired = 1003, //验证码过期
	VerifyCodeErr = 1004, //验证码错误
	UserExist = 1005, //用户已经存在
	PasswdErr = 1006, //密码错误
	EmailNotMatch = 1007, //邮箱不匹配
	PasswdUpFailed = 1008, //更新密码失败
	PasswdInvalid = 1009, //密码更新失败
	TokenInvalid = 1010,   //Token失效
	UidInvalid = 1011,  //uid无效
};

//Defer类(go语言风格的延迟执行)
class Defer {
public:
	//接受一个lambda表达式或者函数指针
	Defer(std::function<void()> func) : func_(func) {}

	//析构时执行传入的函数
	~Defer() {
		func_();
	}

private:
	std::function<void()> func_;
};

#define MAX_LENGTH (1024 * 2) //客户端请求体最大字节数；4 字节包头单独读取
#define MAX_NICK_LEN 20 //昵称最多 20 个字符，界面的输入上限与这里保持一致
#define MAX_DESC_LEN 60 //个性签名最多 60 个字符
#define MAX_TAG_LEN 12 //单个好友标签最多 12 个字符
#define MAX_TAG_COUNT 10 //一个好友最多 10 个标签
#define HEAD_TOTAL_LEN 4 //头部总长度
#define HEAD_ID_LEN 2 //头部id长度
#define HEAD_DATA_LEN 2 //头部数据长度

#define MAX_RECVQUE 10000 //最大接收队列长度
#define MAX_SENDQUE 1000 //最大发送队列长度
#define MAX_LOGICQUE 10000 //最大逻辑队列长度

//按 UTF-8 编码统计字符数: 只数首字节, 后续的 10xxxxxx 续字节跳过
//数据库列宽以字符为单位, 直接比较 std::string 的 size() 会把一个汉字算成 3 个
inline size_t Utf8CharCount(const std::string& text)
{
	size_t count = 0;
	for (unsigned char ch : text) {
		if ((ch & 0xC0) != 0x80)
			++count;
	}
	return count;
}

enum MSG_IDS {
	MSG_CHAT_LOGIN = 1005, //用户登陆
	MSG_CHAT_LOGIN_RSP = 1006, //用户登陆回包
	ID_SEARCH_USER_REQ = 1007, //用户搜索请求
	ID_SEARCH_USER_RSP = 1008, //搜索用户回包
	ID_ADD_FRIEND_REQ = 1009, //申请添加好友请求
	ID_ADD_FRIEND_RSP = 1010, //申请添加好友回复
	ID_NOTIFY_ADD_FRIEND_REQ = 1011,  //通知用户添加好友申请
	ID_AUTH_FRIEND_REQ = 1013, //认证好友请求
	ID_AUTH_FRIEND_RSP = 1014, //认证好友回复
	ID_NOTIFY_AUTH_FRIEND_REQ = 1015, //通知用户认证好友申请
	ID_TEXT_CHAT_MSG_REQ = 1017, //文本聊天信息请求
	ID_TEXT_CHAT_MSG_RSP = 1018, //文本聊天信息回复
	ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, //通知用户文本聊天信息
	ID_NOTIFY_OFF_LINE_REQ = 1021, //通知用户下线
	ID_HEART_BEAT_REQ = 1023, //心跳请求
	ID_HEARTBEAT_RSP = 1024, //心跳回复
	ID_CREATE_PRIVATE_CHAT_REQ = 1025, //打开单聊会话, 服务端按需创建 thread
	ID_CREATE_PRIVATE_CHAT_RSP = 1026, //返回 thread_id 与对方资料
	ID_LOAD_CHAT_THREAD_REQ = 1027, //按 thread_id 游标拉会话列表
	ID_LOAD_CHAT_THREAD_RSP = 1028, //会话列表与 load_more
	ID_LOAD_CHAT_MSG_REQ = 1029, //按 message_id 游标拉会话消息
	ID_LOAD_CHAT_MSG_RSP = 1030, //历史消息与 load_more
	ID_IMG_CHAT_MSG_REQ = 1031, //图片聊天消息: 先落库, 资源随后上传
	ID_IMG_CHAT_MSG_RSP = 1032, //图片聊天消息回执
	ID_NOTIFY_IMG_CHAT_MSG_REQ = 1033, //通知接收方图片已上传完成, 可以下载
	ID_RESOURCE_CHAT_MSG_REQ = 1034, //视频/文件消息: content 存文件名、大小与预览资源
	ID_RESOURCE_CHAT_MSG_RSP = 1035, //视频/文件消息回执
	ID_NOTIFY_RESOURCE_CHAT_MSG_REQ = 1036, //通知接收方视频/文件已上传, 等待手动下载
	ID_UPDATE_PROFILE_REQ = 1045, //修改本人资料: 昵称、性别、个性签名
	ID_UPDATE_PROFILE_RSP = 1046, //修改本人资料回包, 带更新后的完整字段
	ID_UPDATE_FRIEND_REMARK_REQ = 1047, //修改好友备注与标签
	ID_UPDATE_FRIEND_REMARK_RSP = 1048, //备注与标签修改回包
	ID_GET_PROFILE_REQ = 1049, //按uid拉取好友最新资料(昵称/签名/头像/性别)
	ID_GET_PROFILE_RSP = 1050, //最新资料回包
	ID_NOTIFY_PROFILE_CHANGED = 1051, //好友资料变更推送(昵称/签名/头像/性别)
};

#define USERIPPREFIX  "uip_" //用户登录ip server的名字前缀
#define USERTOKENPREFIX  "utoken_" //用户token的名字前缀
#define IPCOUNTPREFIX  "ipcount_" //用户登录ip server的名字前缀
#define USER_BASE_INFO "ubaseinfo_" //用户基本信息的名字前缀
#define LOGIN_COUNT  "logincount" //登录数量的名字
#define NAME_INFO "nameinfo_" //用户名基本信息的名字前缀
#define LOCK_PREFIX "lock_"
#define USER_SESSION_PREFIX "usession_"

#define LOCK_TIME_OUT 10 //分布式锁的持有时间
#define ACQUIRE_TIME_OUT 5 //分布式锁的重试时间
#define HEART_BEAT_INTERVAL 60 //心跳检测间隔
#define HEART_THRESHOLD 20 //心跳阈值
