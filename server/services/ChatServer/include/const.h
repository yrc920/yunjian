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
	RecallExpired = 1012, //超过可撤回时间
	RecallDenied = 1013, //这条消息不允许撤回（不是本人发的或已撤回）
	NotFriend = 1014, //双方已不是好友，消息发不出去
	GroupNotFound = 1015, //群不存在、已解散，或请求者不在群里
	GroupDenied = 1016, //当前角色没有这个操作权限（改群名、踢人、设管理员等）
	GroupFull = 1017, //群成员已达上限
	GroupMemberExists = 1018, //被邀请的人已经在群里
	ServiceUnavailable = 1101, //状态服务暂时不可用，登录可以重试
	SessionFenced = 1102, //会话已被更高 epoch 的新登录顶替
	SessionExpired = 1103, //租约过期
	InstanceUnknown = 1104, //本实例未注册或注册已过期
	TooManyRequests = 1105, //短时间内的登录次数超过上限
	ResponseTooLarge = 1106, //响应超过单帧上限，已回错误包而不是断开连接
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

//消息可撤回时间：超过这个秒数只能留在记录里，与常见 IM 的 2 分钟一致
constexpr int RECALL_WINDOW_SECONDS = 120;
#define MAX_NICK_LEN 20 //昵称最多 20 个字符，界面的输入上限与这里保持一致
#define MAX_DESC_LEN 60 //个性签名最多 60 个字符
#define MAX_TAG_LEN 12 //单个好友标签最多 12 个字符
#define MAX_TAG_COUNT 10 //一个好友最多 10 个标签
#define MAX_GROUP_NAME_LEN 30 //群名称最多 30 个字符，与界面输入上限一致
#define MAX_GROUP_NOTICE_LEN 200 //群公告最多 200 个字符
#define MAX_GROUP_NICK_LEN 16 //我在本群的昵称最多 16 个字符
#define MAX_GROUP_MEMBER 200 //单个群的人数上限，建群邀请与后续加人都按它校验
#define MAX_CREATE_GROUP_MEMBER 50 //一次建群最多勾选的好友数，避免单帧超过 TCP 请求体上限
#define HEAD_TOTAL_LEN 4 //头部总长度
#define HEAD_ID_LEN 2 //头部id长度
#define HEAD_DATA_LEN 2 //头部数据长度

#define MAX_RECVQUE 10000 //最大接收队列长度
#define MAX_SENDQUE 1000 //最大发送队列长度
#define MAX_LOGICQUE 10000 //最大逻辑队列长度

//连接池等待上限：数据库/缓存故障时快速失败，避免业务线程被永久挂住
constexpr int MYSQL_POOL_WAIT_MS = 2000;
//2 字节长度字段的上限；发送缓冲里还要放 4 字节包头，所以再减 4
constexpr std::size_t MAX_RESPONSE_LEN = 65535 - 4;

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
	ID_MARK_READ_REQ = 1052, //上报已读位置
	ID_MARK_READ_RSP = 1053, //已读上报回包
	ID_NOTIFY_MSG_READ = 1054, //对端已读通知, 发送方据此把单勾改成双勾
	ID_RECALL_MSG_REQ = 1055, //撤回自己发的消息
	ID_RECALL_MSG_RSP = 1056, //撤回回包
	ID_NOTIFY_MSG_RECALLED = 1057, //对端撤回通知
	ID_DELETE_FRIEND_REQ = 1058, //删除好友（双向关系一起删，不通知对方）
	ID_DELETE_FRIEND_RSP = 1059, //删除好友回包
	ID_CREATE_GROUP_REQ = 1060, //创建群聊：群名 + 勾选的好友
	ID_CREATE_GROUP_RSP = 1061, //创建结果，带回 thread_id 与群资料
	ID_GET_GROUP_INFO_REQ = 1062, //拉群资料与成员列表
	ID_GET_GROUP_INFO_RSP = 1063, //群资料、成员与我自己的群设置
	ID_UPDATE_GROUP_REQ = 1064, //改群名/公告、改我在本群的昵称、免打扰
	ID_UPDATE_GROUP_RSP = 1065, //群设置修改回包，带回生效后的值
	ID_GROUP_MEMBER_OP_REQ = 1066, //群成员操作：邀请、移除、设/撤管理员、退群、解散
	ID_GROUP_MEMBER_OP_RSP = 1067, //成员操作回包
	ID_NOTIFY_GROUP_CHANGED = 1068, //群资料/成员/免打扰或被移出/解散的推送
	ID_LOGOUT_REQ = 1069, //主动登出：释放租约并吊销令牌
	ID_LOGOUT_RSP = 1070, //登出结果，客户端收到后关闭连接
};

#define USERTOKENPREFIX  "utoken_" //用户token的名字前缀，仅用于登录前的快速校验
#define USER_BASE_INFO "ubaseinfo_" //用户基本信息的名字前缀
#define NAME_INFO "nameinfo_" //用户名基本信息的名字前缀

//租约 90 秒，每 30 秒续租一次：漏一次仍在线，漏两次才被判定过期
constexpr int LEASE_TTL_SECONDS = 90;
constexpr int LEASE_RENEW_SECONDS = 30;
//单次续租携带的会话数，避免一个请求体过大
constexpr int LEASE_RENEW_BATCH = 200;
//投递线程内的跨实例 RPC 截止时间：失败只影响"立刻看到"，消息已在库里，
//不必为单次投递等 3–5 秒，否则队列里的后续消息都被拖慢
constexpr int DELIVERY_RPC_DEADLINE_MS = 800;
//每连接消息令牌桶：20 条/秒、突发 50。正常输入远低于这个值，只拦刷屏
constexpr double MESSAGE_RATE_PER_SECOND = 20.0;
constexpr double MESSAGE_BURST = 50.0;
//路由缓存：在线位置 30 秒、离线 5 秒，离线缓存短一些，对方刚上线就能看见
constexpr int ROUTE_CACHE_SECONDS = 30;
constexpr int ROUTE_NEGATIVE_CACHE_SECONDS = 5;
//私聊投递的负缓存只有 1 秒：一条私聊往往就是"对方刚登录"的第一个信号，
//5 秒窗口会让登录后的第一条消息错过实时投递；群广播仍用 5 秒，避免 200 人风暴
constexpr int ROUTE_SINGLE_NEGATIVE_SECONDS = 1;
constexpr int ROUTE_CACHE_MAX = 20000;

#define HEART_BEAT_INTERVAL 30 //定时器周期：心跳检测、租约续期与实例上报共用
#define HEART_THRESHOLD 20 //心跳阈值
