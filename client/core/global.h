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

//简单的异或加密函数, 输入一个字符串, 返回加密后的字符串
extern std::function<QString(QString)> xorString;

//功能的id
enum ReqId{
    ID_GET_VERIFY_CODE = 1001, //获取验证码
    ID_REG_USER = 1002, //注册用户
    ID_RESET_PWD = 1003, //重置密码
    ID_LOGIN_USER = 1004, //用户登录
    ID_CHAT_LOGIN = 1005, //登陆聊天服务器
    ID_CHAT_LOGIN_RSP = 1006, //登陆聊天服务器回包
    ID_SEARCH_USER_REQ = 1007, //用户搜索请求
    ID_SEARCH_USER_RSP = 1008, //搜索用户回包
    ID_ADD_FRIEND_REQ = 1009,  //添加好友申请
    ID_ADD_FRIEND_RSP = 1010, //申请添加好友回复
    ID_NOTIFY_ADD_FRIEND_REQ = 1011,  //通知用户添加好友申请
    ID_AUTH_FRIEND_REQ = 1013,  //认证好友请求
    ID_AUTH_FRIEND_RSP = 1014,  //认证好友回复
    ID_NOTIFY_AUTH_FRIEND_REQ = 1015, //通知用户认证好友申请
    ID_TEXT_CHAT_MSG_REQ  = 1017,  //文本聊天信息请求
    ID_TEXT_CHAT_MSG_RSP  = 1018,  //文本聊天信息回复
    ID_NOTIFY_TEXT_CHAT_MSG_REQ = 1019, //通知用户文本聊天信息
    ID_NOTIFY_OFF_LINE_REQ = 1021, //通知用户下线
    ID_HEART_BEAT_REQ = 1023,      //心跳请求
    ID_HEARTBEAT_RSP = 1024,       //心跳回复
    ID_CREATE_PRIVATE_CHAT_REQ = 1025, //打开单聊会话
    ID_CREATE_PRIVATE_CHAT_RSP = 1026, //返回 thread_id 与对方资料
    ID_LOAD_CHAT_THREAD_REQ = 1027, //拉会话列表
    ID_LOAD_CHAT_THREAD_RSP = 1028, //会话列表与 load_more
    ID_LOAD_CHAT_MSG_REQ = 1029, //拉会话历史消息
    ID_LOAD_CHAT_MSG_RSP = 1030, //历史消息与 load_more
    ID_IMG_CHAT_MSG_REQ = 1031, //图片聊天消息：先落库，资源随后上传
    ID_IMG_CHAT_MSG_RSP = 1032, //图片聊天消息回执
    ID_NOTIFY_IMG_CHAT_MSG_REQ = 1033, //图片上传完成通知，接收方开始下载
    ID_RESOURCE_CHAT_MSG_REQ = 1034, //视频/文件消息：先落库，资源随后上传
    ID_RESOURCE_CHAT_MSG_RSP = 1035, //视频/文件消息回执
    ID_NOTIFY_RESOURCE_CHAT_MSG_REQ = 1036, //视频/文件上传完成，接收方显示下载卡片
    // 资源服务器沿用 1000 段的系统命令编号，只是另一条连接，帧头为 2 字节 ID + 4 字节长度
    ID_UPLOAD_FILE_REQ = 1037, //通用文件分段上传
    ID_UPLOAD_FILE_RSP = 1038,
    ID_SYNC_FILE_REQ = 1039, //续传前同步服务器进度
    ID_SYNC_FILE_RSP = 1040,
    ID_DOWN_LOAD_FILE_REQ = 1041, //断点下载
    ID_DOWN_LOAD_FILE_RSP = 1042,
    ID_UPLOAD_HEAD_ICON_REQ = 1043, //头像分段上传
    ID_UPLOAD_HEAD_ICON_RSP = 1044,
    ID_UPDATE_PROFILE_REQ = 1045, //修改本人资料：昵称、性别、个性签名
    ID_UPDATE_PROFILE_RSP = 1046, //修改本人资料回包，带更新后的字段
    ID_UPDATE_FRIEND_REMARK_REQ = 1047, //修改好友备注与标签
    ID_UPDATE_FRIEND_REMARK_RSP = 1048, //备注与标签修改回包
    ID_GET_PROFILE_REQ = 1049, //按uid拉取好友最新资料
    ID_GET_PROFILE_RSP = 1050, //最新资料回包
    ID_NOTIFY_PROFILE_CHANGED = 1051, //好友资料变更推送（昵称/签名/头像/性别）
    ID_MARK_READ_REQ = 1052, //上报已读位置
    ID_MARK_READ_RSP = 1053, //已读上报回包
    ID_NOTIFY_MSG_READ = 1054, //对端已读通知，发送方据此把单勾改成双勾
    ID_RECALL_MSG_REQ = 1055, //撤回自己发的消息
    ID_RECALL_MSG_RSP = 1056, //撤回回包
    ID_NOTIFY_MSG_RECALLED = 1057, //对端撤回通知
    ID_DELETE_FRIEND_REQ = 1058, //删除好友（双向关系一起删，不通知对方）
    ID_DELETE_FRIEND_RSP = 1059, //删除好友回包
};

//模块
enum Modules{
    REGISTERMOD = 0, //注册模块
    RESETMOD = 1, //重置密码模块
    LOGINMOD = 2, //登录模块
};

//错误码
enum ErrorCodes{
    SUCCESS = 0, //成功
    ERR_JSON = 1, //json解析失败
    ERR_NETWORK = 2, //网络错误
};

extern QString gate_url_prefix; //网关服务器的URL前缀,从配置文件中读取

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
