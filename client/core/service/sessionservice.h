#ifndef SESSIONSERVICE_H
#define SESSIONSERVICE_H

#include <QObject>
#include <QString>
#include <QTimer>
#include <QJsonObject>
#include <QSet>
#include "global.h"
#include "singleton.h"

/******************************************************************************
 * @file       sessionservice.h
 * @brief      会话服务：建立长连接、完成聊天服务器登录、持有当前会话信息
 *
 * HTTP 登录成功后由 AuthService 交出 ServerInfo，这里负责连 TCP 并发登录帧。
 * 登录回包与通知由这里写入 UserMgr，会话就绪后才通知应用外壳进入聊天页。
 *****************************************************************************/

class SessionService : public QObject, public Singleton<SessionService>
{
    Q_OBJECT

public:
    friend class Singleton<SessionService>;
    ~SessionService();

    // 连接聊天服务器并登录；同一时刻只保留最后一次请求
    void connectToChat(const ServerInfo &info);
    // 主动断开（被踢或异常下线时调用）
    void closeSession();
    // 主动登出：先通知服务端吊销令牌，收到回包或超时后再断开
    void logout();

    bool isReady() const { return _ready; }
    ServerInfo session() const { return _current; }

signals:
    void connecting();                       // 开始连接，界面可显示进度
    void connectFailed(const QString &msg);  // 连接层面失败（超时 / 未连上）
    void sessionReady();                     // 长连接就绪，可以进入聊天界面
    //普通断线后的自动重连：attempt 从 1 开始；成功后发 reconnected
    void reconnecting(int attempt);
    void reconnected();
    //重连到上限或遇到失效凭证：不再重试，交给外壳提示重新登录
    void reconnectFailed(const QString &msg);
    //收到别人发来的新消息：外壳据此弹系统通知；title 已是会话/群名
    void messageArrived(const QString &title, const QString &body,
                        qint64 threadId, int threadType, int peerUid);
    void friendApproved(int uid);            // 别人通过我的申请，界面只更新红点
    void friendAdded(int uid);                // 我自己同意别人，界面直接进入会话

private:
    SessionService();

    void onTcpConnected(bool ok);
    void onChatLoginFailed(int code);
    void onChatLoggedIn(const QJsonObject &data);
    //普通断线：保留账号数据与 token，按退避间隔重建长连接。
    //permanent 为真表示凭证已失效或重试到上限，不再重连
    void scheduleReconnect(const QString &reason, bool permanent = false);
    void startReconnectAttempt();
    void onSessionTimeout();
    void finishLogout();
    void failAndReset(const QString &msg);

    // 拉会话历史；lastMessageId 为 0 表示第一页，否则从这个游标往后取
    void requestMessages(qint64 threadId, qint64 lastMessageId = 0);
    // thread_id 游标翻页：上一页最后一条作为下一页起点
    void loadMoreThreads(qint64 lastThreadId);
    //会话列表全部页到齐：用差集找出服务端已经没有的群（解散/被移出）
    void finishThreadList();

    ServerInfo _current{};
    ServerInfo _pending{};   // 等待连接的会话信息
    bool _ready;
    bool _connecting = false;
    //用户是否还想保持连接：主动退出/被踢时置 false，断线回调据此决定要不要重连
    bool _want_connection = false;
    bool _reconnecting = false;
    int _reconnect_attempt = 0;
    QTimer _reconnect_timer; //退避等待，触发后真正发下一次连接
    QTimer _timeout;       // 连接 + 登录的整体超时，避免界面无限等待
    QTimer _heartbeat;     // 会话建立后的保活
    //心跳看门狗：TCP 没报断开但服务端已经不可达时，靠回包超时触发重连
    QTimer _heartbeat_watchdog;
    QTimer _logout_timer;  // 登出回包超时：控制面不回包也不能把连接一直挂着
    bool _logging_out = false; // 正在等登出回包，避免重复发帧
    //本次登录已经拉取的历史页数。正常登录游标已经在头部，一页就结束；
    //上限只用来拦住异常数据导致的无限翻页
    int _history_pages = 0;
    QSet<qint64> _seen_threads; //本次登录服务端会话列表里出现过的 thread_id
    bool _threads_complete = false;
};

#endif // SESSIONSERVICE_H
