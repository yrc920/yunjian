#include "shellbridge.h"

#include <QDebug>
#include <QSet>

#include "sessionservice.h"
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
            this, [this] { go(kChat); });
    connect(tcp, &TcpMgr::sig_notify_offline, this, [this](int reason) {
        // 被顶号时只提示、不自动重连：旧凭证再登录会把新会话顶回去，形成互踢
        QString text;
        switch (reason) {
        case 2: text = tr("登录状态已过期，请重新登录。"); break;
        case 3: text = tr("服务端已断开该连接，请稍后重试。"); break;
        default: text = tr("同账号异地登录，该终端已下线。"); break;
        }
        showOfflineNotice(text);
        SessionService::GetInstance()->closeSession();
    });
    connect(tcp, &TcpMgr::sig_connection_closed, this, [this] {
        showOfflineNotice(tr("与服务器的连接已断开。"));
    });
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

// 切页；未知目标回落到登录页，避免把窗口留空
void ShellBridge::go(const QString &target)
{
    static const QSet<QString> known = {
        kLogin, QStringLiteral("register"), QStringLiteral("reset"), kChat
    };

    const QString next = known.contains(target) ? target : kLogin;
    if (_page == next)
        return;

    _page = next;
    emit pageChanged();
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

    // 先离开聊天页：关连接会再发一次 sig_connection_closed，此时已不在聊天页，不会重复提示
    go(kLogin);
    SessionService::GetInstance()->closeSession();
}
