#include <QApplication>
#include <QDir>
#include <QFont>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QSettings>
#include <QUrl>

#include "global.h"
#include "authservice.h"
#include "chatservice.h"
#include "fileservice.h"
#include "friendservice.h"
#include "httpmgr.h"
#include "sessionservice.h"
#include "usermgr.h"
#include "localstore.h"
#include "filetcpthread.h"
#include "filetcpmgr.h"
#include "tcpthread.h"
#include "tcpmgr.h"

int main(int argc, char *argv[])
{
    //QApplication 只为 QSystemTrayIcon 提供 QtWidgets 后端；QML/Quick 仍是唯一 UI 层，
    //不创建任何 QWidget 页面。QApplication 是 QGuiApplication 的子类，事件循环不变
    QApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("xiguchat"));
    QGuiApplication::setOrganizationName(QStringLiteral("XiguChat"));
    QFont font = app.font();
#if defined(Q_OS_WIN)
    font.setFamilies({QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Segoe UI")});
#elif defined(Q_OS_MACOS)
    font.setFamilies({QStringLiteral("PingFang SC"), QStringLiteral("Helvetica Neue")});
#else
    font.setFamilies({QStringLiteral("Noto Sans CJK SC"), QStringLiteral("Noto Sans")});
#endif
    app.setFont(font);

    // 统一设置应用窗口图标（三端通用，从 qrc 资源加载）：
    // Windows/Linux 显示在窗口标题栏与任务栏；
    // macOS 窗口标题栏不显示图标，Dock 图标由 .app bundle 内的 .icns 提供（见 CMakeLists APPLE 分支）
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/res/xiguchat.png")));

    // config.ini 与可执行文件同目录（构建后由 CMake 拷到产物目录）
    const QString config_path = QDir(QCoreApplication::applicationDirPath())
                                    .filePath(QStringLiteral("config.ini"));
    QSettings settings(config_path, QSettings::IniFormat);
    // scheme 决定走 HTTP 还是 HTTPS：旧配置没有这一项，按 http 处理，本地开发不用改
    QString scheme = settings.value(QStringLiteral("GateServer/scheme"), QStringLiteral("http"))
                         .toString().trimmed().toLower();
    if (scheme != QStringLiteral("https"))
        scheme = QStringLiteral("http");
    const QString gate_host = settings.value(QStringLiteral("GateServer/host")).toString().trimmed();
    const QString gate_port = settings.value(QStringLiteral("GateServer/port")).toString().trimmed();
    // port 留空表示用协议默认端口（80 / 443），反代到 443 时就不必写端口
    gate_url_prefix = scheme + QStringLiteral("://") + gate_host
                      + (gate_port.isEmpty() ? QString() : QLatin1Char(':') + gate_port);

    // 聊天与文件通道的加密开关：必须与服务端 [SelfServer] 的 SslCert 配置一致
    const QString tls_text = settings.value(QStringLiteral("Network/tls"), QStringLiteral("false"))
                                 .toString().trimmed().toLower();
    tcp_tls_enabled = tls_text == QStringLiteral("true") || tls_text == QStringLiteral("1")
                      || tls_text == QStringLiteral("yes") || tls_text == QStringLiteral("on");

    int exitCode = 0;
    {
        //聊天与资源各用一个网络线程：文件分片不能挤占聊天消息的发送与解析
        TcpThread tcp_thread;
        FileTcpThread file_tcp_thread;

        QQmlApplicationEngine engine;
        engine.load(QUrl(QStringLiteral("qrc:/qt/qml/XiguChat/qml/Main.qml")));
        if (engine.rootObjects().isEmpty())
            exitCode = 1;
        else
            exitCode = app.exec();
    } //先销毁 QML 引擎与网络线程，两个网络管理器在这里被迁回主线程

    //本地缓存库要先 checkpoint 再关连接：放在这里，QGuiApplication 还活着，
    //QSqlDatabase 的清理才安全
    LocalStore::GetInstance()->Close();

    //QObject 单例必须在 QGuiApplication 销毁前释放；否则静态 shared_ptr 会在
    //应用对象析构之后才清理，Qt 内部事件与线程数据已经失效
    SessionService::Destroy();
    ChatService::Destroy();
    FileService::Destroy();
    FriendService::Destroy();
    AuthService::Destroy();
    HttpMgr::Destroy();
    TcpMgr::Destroy();
    FileTcpMgr::Destroy();
    UserMgr::Destroy();
    return exitCode;
}
