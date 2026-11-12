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
    // 资源服务器使用独立的消息空间：帧头为 2 字节 ID + 4 字节长度
    ID_UPLOAD_FILE_REQ = 2001, //通用文件分段上传
    ID_UPLOAD_FILE_RSP = 2002,
    ID_SYNC_FILE_REQ = 2003, //续传前同步服务器进度
    ID_SYNC_FILE_RSP = 2004,
    ID_DOWN_LOAD_FILE_REQ = 2005, //断点下载
    ID_DOWN_LOAD_FILE_RSP = 2006,
    ID_UPLOAD_HEAD_ICON_REQ = 2007, //头像分段上传
    ID_UPLOAD_HEAD_ICON_RSP = 2008,
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
