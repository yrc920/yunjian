#ifndef SHELLBRIDGE_H
#define SHELLBRIDGE_H

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

/**
 * @brief 应用外壳：唯一持有「当前显示哪一页」的地方
 *
 * 页面自身不保存导航状态，QML 里调 `go("register")`，这里改 page，
 * 根窗口的 Loader 随之换页并调整窗口尺寸。
 *
 * 掉线通知也在这里：服务端踢人、长连接断开时给出提示，
 * 用户确认后关闭会话并回到登录页。
 */
class ShellBridge : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString page READ page NOTIFY pageChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    //普通断线后的自动重连状态：聊天页顶部显示一条非阻塞横条
    Q_PROPERTY(bool reconnecting READ reconnecting NOTIFY reconnectingChanged)
    Q_PROPERTY(QString reconnectText READ reconnectText NOTIFY reconnectingChanged)
    //托盘提示与跨平台未读展示：Dock 角标只在 macOS 生效，Windows/Linux 靠这里兜底
    Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadTotalChanged)

public:
    explicit ShellBridge(QObject *parent = nullptr);
    ~ShellBridge() override;

    // 根窗口里只有一个实例；页面桥接对象通过它导航
    static ShellBridge *instance();

    QString page() const { return _page; }
    QString notice() const { return _notice; }
    bool reconnecting() const { return _reconnecting; }
    QString reconnectText() const { return _reconnectText; }
    int unreadTotal() const;

    Q_INVOKABLE void go(const QString &target);
    Q_INVOKABLE void dismissNotice();
    // 主动退出登录：断开长连接、清空账号数据、回到登录页
    Q_INVOKABLE void logout();

signals:
    void pageChanged();
    void noticeChanged();
    void reconnectingChanged();
    void unreadTotalChanged();
    //新消息：Main.qml 用它弹系统通知；点击通知可打开对应会话
    void messageArrived(const QString &title, const QString &body,
                        qint64 threadId, int threadType, int peerUid);

private:
    void showOfflineNotice(const QString &message);
    void setReconnecting(bool active, const QString &text = {});

    QString _page = QStringLiteral("login");
    QString _notice;
    bool _reconnecting = false;
    QString _reconnectText;
};

#endif // SHELLBRIDGE_H
