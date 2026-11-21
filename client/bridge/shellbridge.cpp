#include "shellbridge.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QSet>

#include "sessionservice.h"
#include "autologinstore.h"
#include "tcpmgr.h"
#include "usermgr.h"

namespace {

ShellBridge *g_shell = nullptr;

const QString kLogin = QStringLiteral("login");
const QString kChat = QStringLiteral("chat");

} // namespace

ShellBridge::ShellBridge(QObject *parent) : QObject(parent)
{
    g_shell = this;

    auto *tcp = TcpMgr::GetInstance().get();
    connect(SessionService::GetInstance().get(), &SessionService::sessionReady,
            this, [this] {
                setReconnecting(false);
                go(kChat);
            });
    connect(SessionService::GetInstance().get(), &SessionService::reconnecting,
            this, [this](int attempt) {
                setReconnecting(true, tr("连接已断开，正在重连…（第 %1 次）").arg(attempt));
            });
    connect(SessionService::GetInstance().get(), &SessionService::reconnected,
            this, [this] { setReconnecting(false); });
    connect(SessionService::GetInstance().get(), &SessionService::reconnectFailed,
            this, [this](const QString &message) {
                setReconnecting(false);
                showOfflineNotice(message);
            });
    //新消息通知：已经在这条会话里看的时候不打扰；页面不在聊天页也不弹
    connect(SessionService::GetInstance().get(), &SessionService::messageArrived, this,
            [this](const QString &title, const QString &body,
                   qint64 threadId, int threadType, int peerUid) {
                if (_page != kChat || threadId <= 0)
                    return;
                if (UserMgr::GetInstance()->ActiveThread() == threadId)
                    return;
                //免打扰的会话不弹系统通知，会话行与角标仍然自己更新
                if (UserMgr::GetInstance()->ThreadMuted(threadId))
                    return;
                emit messageArrived(title, body, threadId, threadType, peerUid);
            });
    //Dock/任务栏角标跟着未读总数走；免打扰会话已经在 UnreadBadgeTotal 里跳过
    auto *userMgr = UserMgr::GetInstance().get();
    connect(userMgr, &UserMgr::UnreadChanged, this, [this, userMgr] {
        if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
            app->setBadgeNumber(userMgr->UnreadBadgeTotal());
        emit unreadTotalChanged();
    });
    connect(userMgr, &UserMgr::SessionCleared, this, [this] {
        if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
            app->setBadgeNumber(0);
        emit unreadTotalChanged();
    });
    connect(tcp, &TcpMgr::sig_notify_offline, this, [this](int reason) {
        // 被顶号时只提示、不自动重连：旧凭证再登录会把新会话顶回去，形成互踢
        setReconnecting(false);
        QString text;
        switch (reason) {
        case 2: text = tr("登录状态已过期，请重新登录。"); break;
        case 3: text = tr("服务端已断开该连接，请稍后重试。"); break;
        default: text = tr("同账号异地登录，该终端已下线。"); break;
        }
        showOfflineNotice(text);
        SessionService::GetInstance()->closeSession();
    });
    //普通断线不再直接弹"连接中断"：SessionService 会先自动重连，
    //连续失败或凭证失效时才通过 reconnectFailed 进入弹层
}

ShellBridge::~ShellBridge()
{
    if (g_shell == this)
        g_shell = nullptr;
}

ShellBridge *ShellBridge::instance()
{
    return g_shell;
}

int ShellBridge::unreadTotal() const
{
    return UserMgr::GetInstance()->UnreadBadgeTotal();
}

// 切页；未知目标回落到登录页，避免把窗口留空
void ShellBridge::go(const QString &target)
{
    static const QSet<QString> known = {
        kLogin, QStringLiteral("register"), QStringLiteral("reset"), kChat
    };

    const QString next = known.contains(target) ? target : kLogin;
    if (_page == next)
        return;

    //离开聊天页就不再显示重连横条
    if (next == kLogin)
        setReconnecting(false);
    _page = next;
    emit pageChanged();
}

void ShellBridge::setReconnecting(bool active, const QString &text)
{
    const QString next = active ? text : QString();
    if (_reconnecting == active && _reconnectText == next)
        return;
    _reconnecting = active;
    _reconnectText = next;
    emit reconnectingChanged();
}

// 只有停在聊天页时才提示掉线，同一时间只提示一次
void ShellBridge::showOfflineNotice(const QString &message)
{
    // 登录页本来就没建立会话，不必打扰；已经提示过的不重复弹
    if (_page != kChat || !_notice.isEmpty())
        return;

    _notice = message;
    emit noticeChanged();
}

// 主动退出：与掉线不同的是不会弹提示，直接回登录页。
// 先切页再关连接，避免 closeSession 触发的断线信号被当成异常提示
void ShellBridge::logout()
{
    _notice.clear();
    emit noticeChanged();
    setReconnecting(false);
    // 用户主动退出：连同自动登录凭据一起清掉，下次回到登录页
    AutoLoginStore::Clear();
    go(kLogin);
    // 明确退出要通知服务端吊销令牌；closeSession 只负责最终断开
    SessionService::GetInstance()->logout();
    UserMgr::GetInstance()->ClearSession();
}

// 关提示、回登录页、结束会话
void ShellBridge::dismissNotice()
{
    _notice.clear();
    emit noticeChanged();
    setReconnecting(false);

    // 先离开聊天页：关连接会再发一次 sig_connection_closed，此时已不在聊天页，不会重复提示
    go(kLogin);
    SessionService::GetInstance()->closeSession();
}
