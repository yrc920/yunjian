#ifndef GLOBAL_H
#define GLOBAL_H

#include <functional>
#include <QString>
#include <QMetaType>
/******************************************************************************
 *
 * @file       global.h
 * @brief      全局变量和枚举
 *
 * @author     西故
 * @date       2026/04/20
 * @history
 *****************************************************************************/

//密码发送前的可逆变形: 登录、注册、重置密码三条路径都必须先过一遍。
//网关拿到字符串后直接做 Argon2 哈希, 库里存的也是变形值的哈希, 两端不能有一边发原文
extern std::function<QString(QString)> xorString;

//功能的id
//长连接消息按职责分段，每段内请求/响应相邻，响应固定为请求 + 1：
//  2xxx 连接与会话    3xxx 用户与好友    4xxx 会话与消息
//  5xxx 群聊          6xxx 资源通道（独立连接，帧头为 2+4 字节）
//1001–1004 只用于 HTTP 网关内部关联，不上长连接
enum ReqId{
    ID_GET_VERIFY_CODE = 1001, //获取验证码
    ID_REG_USER = 1002, //注册用户
    ID_RESET_PWD = 1003, //重置密码
    ID_LOGIN_USER = 1004, //用户登录

    //2xxx：连接、认证与保活
    ID_CHAT_LOGIN = 2001, //登陆聊天服务器
    ID_CHAT_LOGIN_RSP = 2002, //登陆聊天服务器回包
    ID_HEART_BEAT_REQ = 2003, //心跳请求
    ID_HEARTBEAT_RSP = 2004, //心跳回复
    ID_LOGOUT_REQ = 2005, //主动登出：释放租约并吊销令牌
    ID_LOGOUT_RSP = 2006, //登出结果，收到后关闭连接
    ID_NOTIFY_OFF_LINE_REQ = 2007, //通知用户下线（服务端推送）

    //3xxx：用户搜索、好友关系与资料
    ID_SEARCH_USER_REQ = 3001, //用户搜索请求
    ID_SEARCH_USER_RSP = 3002, //搜索用户回包
    ID_ADD_FRIEND_REQ = 3003, //添加好友申请
    ID_ADD_FRIEND_RSP = 3004, //申请添加好友回复
    ID_NOTIFY_ADD_FRIEND_REQ = 3005, //通知用户添加好友申请
    ID_AUTH_FRIEND_REQ = 3006, //认证好友请求
    ID_AUTH_FRIEND_RSP = 3007, //认证好友回复
    ID_NOTIFY_AUTH_FRIEND_REQ = 3008, //通知用户认证好友申请
    ID_DELETE_FRIEND_REQ = 3009, //删除好友（只删自己这一侧，不通知对方）
    ID_DELETE_FRIEND_RSP = 3010, //删除好友回包
    ID_UPDATE_FRIEND_REMARK_REQ = 3011, //修改好友备注与标签
    ID_UPDATE_FRIEND_REMARK_RSP = 3012, //备注与标签修改回包
    ID_GET_PROFILE_REQ = 3013, //按uid拉取好友最新资料
    ID_GET_PROFILE_RSP = 3014, //最新资料回包
    ID_NOTIFY_PROFILE_CHANGED = 3015, //好友资料变更推送
    ID_UPDATE_PROFILE_REQ = 3016, //修改本人资料：昵称、性别、个性签名
    ID_UPDATE_PROFILE_RSP = 3017, //修改本人资料回包

    //4xxx：单聊会话、消息、已读与撤回
    ID_CREATE_PRIVATE_CHAT_REQ = 4001, //打开单聊会话
    ID_CREATE_PRIVATE_CHAT_RSP = 4002, //返回 thread_id 与对方资料
    ID_LOAD_CHAT_THREAD_REQ = 4003, //拉会话列表
    ID_LOAD_CHAT_THREAD_RSP = 4004, //会话列表与 load_more
    ID_LOAD_CHAT_MSG_REQ = 4005, //拉会话历史消息
    ID_LOAD_CHAT_MSG_RSP = 4006, //历史消息与 load_more
    ID_TEXT_CHAT_MSG_REQ = 4007, //文本聊天信息请求
    ID_TEXT_CHAT_MSG_RSP = 4008, //文本聊天信息回复
    ID_NOTIFY_TEXT_CHAT_MSG_REQ = 4009, //通知用户文本聊天信息
    ID_IMG_CHAT_MSG_REQ = 4010, //图片聊天消息：先落库，资源随后上传
    ID_IMG_CHAT_MSG_RSP = 4011, //图片聊天消息回执
    ID_NOTIFY_IMG_CHAT_MSG_REQ = 4012, //图片上传完成通知，接收方开始下载
    ID_RESOURCE_CHAT_MSG_REQ = 4013, //视频/文件消息：先落库，资源随后上传
    ID_RESOURCE_CHAT_MSG_RSP = 4014, //视频/文件消息回执
    ID_NOTIFY_RESOURCE_CHAT_MSG_REQ = 4015, //视频/文件上传完成，接收方显示卡片
    ID_MARK_READ_REQ = 4016, //上报已读位置
    ID_MARK_READ_RSP = 4017, //已读上报回包
    ID_NOTIFY_MSG_READ = 4018, //对端已读通知，发送方据此把单勾改成双勾
    ID_RECALL_MSG_REQ = 4019, //撤回自己发的消息
    ID_RECALL_MSG_RSP = 4020, //撤回回包
    ID_NOTIFY_MSG_RECALLED = 4021, //对端撤回通知

    //5xxx：群聊资料、设置与成员管理
    ID_CREATE_GROUP_REQ = 5001, //创建群聊：群名 + 勾选的好友
    ID_CREATE_GROUP_RSP = 5002, //创建结果，带回 thread_id 与群资料
    ID_GET_GROUP_INFO_REQ = 5003, //拉群资料与成员列表
    ID_GET_GROUP_INFO_RSP = 5004, //群资料、成员与我自己的群设置
    ID_UPDATE_GROUP_REQ = 5005, //改群名/公告、改我在本群的昵称、免打扰
    ID_UPDATE_GROUP_RSP = 5006, //群设置修改回包
    ID_GROUP_MEMBER_OP_REQ = 5007, //邀请、移除、设/撤管理员、退群、解散
    ID_GROUP_MEMBER_OP_RSP = 5008, //成员操作回包
    ID_NOTIFY_GROUP_CHANGED = 5009, //群资料/成员/被移出/解散的推送

    //6xxx：资源通道，另一条 TCP 连接，帧头为 2 字节 ID + 4 字节长度
    ID_UPLOAD_FILE_REQ = 6001, //通用文件分段上传
    ID_UPLOAD_FILE_RSP = 6002,
    ID_SYNC_FILE_REQ = 6003, //续传前同步服务器进度
    ID_SYNC_FILE_RSP = 6004,
    ID_DOWN_LOAD_FILE_REQ = 6005, //断点下载
    ID_DOWN_LOAD_FILE_RSP = 6006,
    ID_UPLOAD_HEAD_ICON_REQ = 6007, //头像分段上传
    ID_UPLOAD_HEAD_ICON_RSP = 6008,
};

//请求与响应必须相邻：服务端用 request + 1 回包，客户端按同一规则注册处理函数
#define XIGU_PROTOCOL_PAIR(req, rsp) \
    static_assert(static_cast<int>(rsp) == static_cast<int>(req) + 1, #req " 与 " #rsp " 必须相邻")
XIGU_PROTOCOL_PAIR(ID_CHAT_LOGIN, ID_CHAT_LOGIN_RSP);
XIGU_PROTOCOL_PAIR(ID_HEART_BEAT_REQ, ID_HEARTBEAT_RSP);
XIGU_PROTOCOL_PAIR(ID_LOGOUT_REQ, ID_LOGOUT_RSP);
XIGU_PROTOCOL_PAIR(ID_SEARCH_USER_REQ, ID_SEARCH_USER_RSP);
XIGU_PROTOCOL_PAIR(ID_ADD_FRIEND_REQ, ID_ADD_FRIEND_RSP);
XIGU_PROTOCOL_PAIR(ID_AUTH_FRIEND_REQ, ID_AUTH_FRIEND_RSP);
XIGU_PROTOCOL_PAIR(ID_DELETE_FRIEND_REQ, ID_DELETE_FRIEND_RSP);
XIGU_PROTOCOL_PAIR(ID_UPDATE_FRIEND_REMARK_REQ, ID_UPDATE_FRIEND_REMARK_RSP);
XIGU_PROTOCOL_PAIR(ID_GET_PROFILE_REQ, ID_GET_PROFILE_RSP);
XIGU_PROTOCOL_PAIR(ID_UPDATE_PROFILE_REQ, ID_UPDATE_PROFILE_RSP);
XIGU_PROTOCOL_PAIR(ID_CREATE_PRIVATE_CHAT_REQ, ID_CREATE_PRIVATE_CHAT_RSP);
XIGU_PROTOCOL_PAIR(ID_LOAD_CHAT_THREAD_REQ, ID_LOAD_CHAT_THREAD_RSP);
XIGU_PROTOCOL_PAIR(ID_LOAD_CHAT_MSG_REQ, ID_LOAD_CHAT_MSG_RSP);
XIGU_PROTOCOL_PAIR(ID_TEXT_CHAT_MSG_REQ, ID_TEXT_CHAT_MSG_RSP);
XIGU_PROTOCOL_PAIR(ID_IMG_CHAT_MSG_REQ, ID_IMG_CHAT_MSG_RSP);
XIGU_PROTOCOL_PAIR(ID_RESOURCE_CHAT_MSG_REQ, ID_RESOURCE_CHAT_MSG_RSP);
XIGU_PROTOCOL_PAIR(ID_MARK_READ_REQ, ID_MARK_READ_RSP);
XIGU_PROTOCOL_PAIR(ID_RECALL_MSG_REQ, ID_RECALL_MSG_RSP);
XIGU_PROTOCOL_PAIR(ID_CREATE_GROUP_REQ, ID_CREATE_GROUP_RSP);
XIGU_PROTOCOL_PAIR(ID_GET_GROUP_INFO_REQ, ID_GET_GROUP_INFO_RSP);
XIGU_PROTOCOL_PAIR(ID_UPDATE_GROUP_REQ, ID_UPDATE_GROUP_RSP);
XIGU_PROTOCOL_PAIR(ID_GROUP_MEMBER_OP_REQ, ID_GROUP_MEMBER_OP_RSP);
XIGU_PROTOCOL_PAIR(ID_UPLOAD_FILE_REQ, ID_UPLOAD_FILE_RSP);
XIGU_PROTOCOL_PAIR(ID_SYNC_FILE_REQ, ID_SYNC_FILE_RSP);
XIGU_PROTOCOL_PAIR(ID_DOWN_LOAD_FILE_REQ, ID_DOWN_LOAD_FILE_RSP);
XIGU_PROTOCOL_PAIR(ID_UPLOAD_HEAD_ICON_REQ, ID_UPLOAD_HEAD_ICON_RSP);
#undef XIGU_PROTOCOL_PAIR

//模块
enum Modules{
    REGISTERMOD = 0, //注册模块
    RESETMOD = 1, //重置密码模块
    LOGINMOD = 2, //登录模块
};

//客户端本地错误码：只表示"请求没有到达业务层"
enum ErrorCodes{
    SUCCESS = 0, //成功
    ERR_JSON = 1, //json解析失败
    ERR_NETWORK = 2, //网络错误
};

//服务端业务错误码：与 server/common/include/Protocol.h 保持一致。
//新增错误码时两端同步，界面文案只在这里做一次映射
namespace server_error {
constexpr int Json = 1001;
constexpr int RpcFailed = 1002;
constexpr int VerifyExpired = 1003;
constexpr int VerifyCodeErr = 1004;
constexpr int UserExist = 1005;
constexpr int PasswdErr = 1006;
constexpr int EmailNotMatch = 1007;
constexpr int PasswdUpFailed = 1008;
constexpr int PasswdInvalid = 1009;
constexpr int TokenInvalid = 1010;
constexpr int UidInvalid = 1011;
constexpr int RecallExpired = 1012;
constexpr int RecallDenied = 1013;
constexpr int NotFriend = 1014;
constexpr int GroupNotFound = 1015;
constexpr int GroupDenied = 1016;
constexpr int GroupFull = 1017;
constexpr int GroupMemberExists = 1018;
constexpr int VerifyTooFrequent = 1022;
constexpr int VerifySendFailed = 1023;
constexpr int ServiceUnavailable = 1101;
constexpr int SessionFenced = 1102;
constexpr int SessionExpired = 1103;
constexpr int InstanceUnknown = 1104;
constexpr int TooManyRequests = 1105;
constexpr int ResponseTooLarge = 1106;
//资源通道 12xx
constexpr int FileNotExists = 1201;
constexpr int FileSaveRedisFailed = 1202;
constexpr int FileWritePermissionFailed = 1203;
constexpr int FileDataInvalid = 1204;
constexpr int FileNameInvalid = 1205;
constexpr int FileTooLarge = 1206;
constexpr int FileSeqInvalid = 1207;
constexpr int FileReadPermissionFailed = 1208;
constexpr int FileOffsetInvalid = 1209;
constexpr int FileQuotaExceeded = 1210;
} // namespace server_error

extern QString gate_url_prefix; //网关服务器的URL前缀,从配置文件中读取
//聊天与文件通道是否走 TLS：必须与服务端证书配置一致，网关协议由 gate_url_prefix 的 scheme 决定
extern bool tcp_tls_enabled;

//服务器信息结构体
struct ServerInfo{
    //跨线程投递要求默认构造与可拷贝
    ServerInfo() = default;
    QString Host; //服务器地址
    QString Port; //服务器端口
    QString Token; //用户令牌
    int Uid = 0; //用户ID
    QString ResHost; //资源服务器地址，未配置时为空
    QString ResPort; //资源服务器端口
};

Q_DECLARE_METATYPE(ServerInfo)
Q_DECLARE_METATYPE(ReqId)

#endif // GLOBAL_H
