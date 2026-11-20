#pragma once
// 常量定义和工具类
//

#include <string>
#include "Protocol.h"

#include "Defer.h"

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

//长连接消息 ID 按职责分段，每段内请求/响应相邻：
//  2xxx 连接与会话    3xxx 用户与好友    4xxx 会话与消息
//  5xxx 群聊          6xxx 资源通道（另一条 TCP 连接，帧头为 2+4 字节）
//1000 段留给 HTTP 网关操作与业务错误码，不再与长连接消息混用
enum MSG_IDS {
	//2xxx：连接、认证与保活
	MSG_CHAT_LOGIN = 2001, //用户登陆
	MSG_CHAT_LOGIN_RSP = 2002, //用户登陆回包
	ID_HEART_BEAT_REQ = 2003, //心跳请求
	ID_HEARTBEAT_RSP = 2004, //心跳回复
	ID_LOGOUT_REQ = 2005, //主动登出：释放租约并吊销令牌
	ID_LOGOUT_RSP = 2006, //登出结果，客户端收到后关闭连接
	ID_NOTIFY_OFF_LINE_REQ = 2007, //通知用户下线（服务端推送，无响应）

	//3xxx：用户搜索、好友关系与资料
	ID_SEARCH_USER_REQ = 3001, //用户搜索请求
	ID_SEARCH_USER_RSP = 3002, //搜索用户回包
	ID_ADD_FRIEND_REQ = 3003, //申请添加好友请求
	ID_ADD_FRIEND_RSP = 3004, //申请添加好友回复
	ID_NOTIFY_ADD_FRIEND_REQ = 3005, //通知用户添加好友申请（推送）
	ID_AUTH_FRIEND_REQ = 3006, //认证好友请求
	ID_AUTH_FRIEND_RSP = 3007, //认证好友回复
	ID_NOTIFY_AUTH_FRIEND_REQ = 3008, //通知用户认证好友申请（推送）
	ID_DELETE_FRIEND_REQ = 3009, //删除好友（只删自己这一侧，不通知对方）
	ID_DELETE_FRIEND_RSP = 3010, //删除好友回包
	ID_UPDATE_FRIEND_REMARK_REQ = 3011, //修改好友备注与标签
	ID_UPDATE_FRIEND_REMARK_RSP = 3012, //备注与标签修改回包
	ID_GET_PROFILE_REQ = 3013, //按uid拉取好友最新资料（昵称/签名/头像/性别）
	ID_GET_PROFILE_RSP = 3014, //最新资料回包
	ID_NOTIFY_PROFILE_CHANGED = 3015, //好友资料变更推送（推送）
	ID_UPDATE_PROFILE_REQ = 3016, //修改本人资料：昵称、性别、个性签名
	ID_UPDATE_PROFILE_RSP = 3017, //修改本人资料回包，带更新后的完整字段

	//4xxx：单聊会话、消息、已读与撤回
	ID_CREATE_PRIVATE_CHAT_REQ = 4001, //打开单聊会话，服务端按需创建 thread
	ID_CREATE_PRIVATE_CHAT_RSP = 4002, //返回 thread_id 与对方资料
	ID_LOAD_CHAT_THREAD_REQ = 4003, //按复合游标拉会话列表
	ID_LOAD_CHAT_THREAD_RSP = 4004, //会话列表与 load_more
	ID_LOAD_CHAT_MSG_REQ = 4005, //按 message_id 游标拉会话消息
	ID_LOAD_CHAT_MSG_RSP = 4006, //历史消息与 load_more
	ID_TEXT_CHAT_MSG_REQ = 4007, //文本聊天信息请求
	ID_TEXT_CHAT_MSG_RSP = 4008, //文本聊天信息回复
	ID_NOTIFY_TEXT_CHAT_MSG_REQ = 4009, //通知用户文本聊天信息（推送）
	ID_IMG_CHAT_MSG_REQ = 4010, //图片消息：先落库，资源随后上传
	ID_IMG_CHAT_MSG_RSP = 4011, //图片消息回执
	ID_NOTIFY_IMG_CHAT_MSG_REQ = 4012, //通知接收方图片已上传完成（推送）
	ID_RESOURCE_CHAT_MSG_REQ = 4013, //视频/文件消息：content 存名称、大小与预览资源
	ID_RESOURCE_CHAT_MSG_RSP = 4014, //视频/文件消息回执
	ID_NOTIFY_RESOURCE_CHAT_MSG_REQ = 4015, //通知接收方视频/文件已上传（推送）
	ID_MARK_READ_REQ = 4016, //上报已读位置
	ID_MARK_READ_RSP = 4017, //已读上报回包
	ID_NOTIFY_MSG_READ = 4018, //对端已读通知（推送）
	ID_RECALL_MSG_REQ = 4019, //撤回自己发的消息
	ID_RECALL_MSG_RSP = 4020, //撤回回包
	ID_NOTIFY_MSG_RECALLED = 4021, //对端撤回通知（推送）

	//5xxx：群聊资料、设置与成员管理
	ID_CREATE_GROUP_REQ = 5001, //创建群聊：群名 + 勾选的好友
	ID_CREATE_GROUP_RSP = 5002, //创建结果，带回 thread_id 与群资料
	ID_GET_GROUP_INFO_REQ = 5003, //拉群资料与成员列表
	ID_GET_GROUP_INFO_RSP = 5004, //群资料、成员与我自己的群设置
	ID_UPDATE_GROUP_REQ = 5005, //改群名/公告、改我在本群的昵称、免打扰
	ID_UPDATE_GROUP_RSP = 5006, //群设置修改回包
	ID_GROUP_MEMBER_OP_REQ = 5007, //群成员操作：邀请、移除、设/撤管理员、退群、解散
	ID_GROUP_MEMBER_OP_RSP = 5008, //成员操作回包
	ID_NOTIFY_GROUP_CHANGED = 5009, //群资料/成员/被移出/解散的推送
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

//消息归档：默认保留一年，每小时检查一次（默认 24 小时执行一批），
//每批 2000 条，避免一次删太多把表锁住
constexpr int DEFAULT_ARCHIVE_RETENTION_DAYS = 365;
constexpr int DEFAULT_ARCHIVE_INTERVAL_HOURS = 24;
constexpr int DEFAULT_ARCHIVE_BATCH = 2000;
//第一次归档前的等待时间（秒）
constexpr int DEFAULT_ARCHIVE_FIRST_DELAY_SECONDS = 300;

#define HEART_BEAT_INTERVAL 30 //定时器周期：心跳检测、租约续期与实例上报共用
#define HEART_THRESHOLD 20 //心跳阈值
