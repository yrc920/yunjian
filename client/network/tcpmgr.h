#ifndef TCPMGR_H
#define TCPMGR_H

#include <QSslSocket>
#include <QQueue>
#include <QThread>
#include "singleton.h"
#include "global.h"
#include <QObject>
#include "userdata.h"
/******************************************************************************
 *
 * @file       tcpmgr.h
 * @brief      聊天服务器 TCP 管理者，运行在独立网络线程
 *
 * @author     西故
 * @date       2026/04/29
 * @history
 *****************************************************************************/

class TcpMgr : public QObject, public Singleton<TcpMgr>,
    public std::enable_shared_from_this<TcpMgr>
{
    Q_OBJECT //使得TcpMgr可以使用Qt的信号和槽机制

public:
    ~TcpMgr(); //使基类析构时可以访问派生类的析构函数
    //由TcpThread在线程启动后调用：QObject与成员socket都要跟随到网络线程
    void MoveToThread(QThread *thread);

    //socket 是私有成员，不能跨线程直接操作；连接与关闭统一走信号
    void ConnectToServer(const ServerInfo &info);
    void CloseConnectionAsync();

private:
    friend class Singleton<TcpMgr>; //使基类创建实例时可以访问私有构造函数
    TcpMgr();
    void initHandlers(); //初始化消息处理函数
    void handleMsg(ReqId id, int len, QByteArray data); //处理消息的函数
    void registerMetaType(); //注册跨线程信号携带的自定义类型
    void resetRecvState(); //清空半包缓冲与解析游标，断开连接时必须调用

    QSslSocket _socket; //明文与 TLS 都走它，必须与TcpMgr在同一线程
    QString _host; //服务器地址
    uint16_t _port; //服务器端口
    QByteArray _buffer; //接收数据的缓冲区
    bool _b_recv_pending; //是否正在等待接收完整消息的标志
    quint16 _message_id; //当前消息的ID
    quint16 _message_len; //当前消息的长度
    //消息ID到处理函数的映射
    QMap<ReqId, std::function<void(ReqId id, int len, QByteArray data)>> _handlers;

    //发送队列：socket.write可能因缓冲区满返回-1，统一由bytesWritten续写
    QQueue<QByteArray> _send_queue;
    QByteArray _current_block;
    qint64 _bytes_sent = 0;
    bool _pending = false;
    //区分主动重连与对端断开：主动close不应被会话层当成登录失败
    bool _manual_close = false;

public slots:
    void slot_tcp_connect(ServerInfo info); //连接服务器的槽函数
    void slot_send_data(ReqId reqId, QByteArray data); //发送数据的槽函数
    void CloseConnection(); //实际关闭socket，只在网络线程执行

signals:
    void sig_connect_tcp(ServerInfo info); //请求连接，跨线程投递
    void sig_close_connection(); //请求关闭
    void sig_con_success(bool bsuccess); //连接成功的信号
    void sig_con_failed(const QString &message); //连接失败（DNS/拒绝/超时），带可展示的原因
    void sig_send_data(ReqId reqId, QByteArray data); //发送数据的信号
    void sig_login_success(const QJsonObject &data); //聊天服务登录数据，由会话层接收
    void sig_login_failed(int); //登录失败的信号，参数为错误码
    void sig_user_search(std::shared_ptr<SearchInfo>); //用户搜索结果的信号
    void sig_friend_apply(std::shared_ptr<AddFriendApply>); //申请对方为好友的信号
    void sig_add_friend_rsp(const QJsonObject &data); //1010 申请回执，失败也要交给界面判断
    void sig_auth_rsp(std::shared_ptr<AuthRsp>); //1015 别人通过我的申请
    //1014 我自己同意别人：与 1015 分开处理，我要跳进会话，对方只需要红点
    void sig_auth_friend_rsp(std::shared_ptr<AuthRsp>);
    void sig_text_chat_msg(std::shared_ptr<TextChatMsg>); //收到文本消息的信号
    void sig_chat_msg_failed(const QJsonObject &data); //1018 回执带错误，按 msgid 标失败
    void sig_chat_threads(const QJsonObject &data); //1028 会话列表，含 load_more
    void sig_chat_messages(const QJsonObject &data); //1030 历史消息，含 load_more
    void sig_chat_img_rsp(const QJsonObject &data); //1032 图片消息回执，成功后才上传资源
    void sig_image_notify(const QJsonObject &data); //1033 对方图片已上传，开始下载
    void sig_resource_chat_rsp(const QJsonObject &data); //1035 视频/文件回执
    void sig_resource_notify(const QJsonObject &data); //1036 对方视频/文件已上传，等用户下载
    void sig_private_chat_created(const QJsonObject &data); //1026 建会话结果，error 由会话层判断
    void sig_profile_updated(const QJsonObject &data); //1046 本人资料修改结果，失败也回传
    void sig_friend_remark_updated(const QJsonObject &data); //1048 好友备注与标签修改结果
    void sig_profile_loaded(const QJsonObject &data); //1050 好友最新资料，失败也回传
    void sig_friend_profile_changed(const QJsonObject &data); //1051 好友资料变更推送
    void sig_mark_read_rsp(const QJsonObject &data); //1053 已读上报回包
    void sig_message_read(const QJsonObject &data); //1054 对端已读，把单勾改成双勾
    void sig_recall_rsp(const QJsonObject &data); //1056 撤回回包
    void sig_message_recalled(const QJsonObject &data); //1057 对端撤回
    void sig_delete_friend_rsp(const QJsonObject &data); //1059 删除好友回包
    //群聊：建群、群资料、群设置、成员操作与变更推送
    void sig_group_created(const QJsonObject &data); //1061 建群结果
    void sig_group_info(const QJsonObject &data); //1063 群资料与成员
    void sig_group_updated(const QJsonObject &data); //1065 群设置修改结果
    void sig_group_member_op(const QJsonObject &data); //1067 成员操作结果
    void sig_group_changed(const QJsonObject &data); //1068 群资料/成员/被移出/解散
    void sig_notify_offline(int reason); //收到服务器踢人消息：1 异地登录 / 2 登录过期 / 3 服务端下线
    void sig_logout_rsp(); //登出回包：服务端已吊销租约与令牌
    void sig_heartbeat_ok(); //心跳回包：会话层用它喂看门狗，发现"静默断网"
    void sig_connection_closed(); //连接断开的信号
};

#endif // TCPMGR_H
