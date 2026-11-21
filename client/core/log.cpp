#include "log.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QStandardPaths>
#include <QtGlobal>

#include <cstdio>

Q_LOGGING_CATEGORY(lcNet, "xc.net")
Q_LOGGING_CATEGORY(lcStore, "xc.store")
Q_LOGGING_CATEGORY(lcApp, "xc.app")

namespace {

constexpr qint64 kMaxFileSize = 8 * 1024 * 1024; //单文件上限，超过就滚动
constexpr int kMaxBackups = 3;                   //保留 client.1..3.log
constexpr int kLevelOff = 4;
constexpr int kLevelDebug = 0;

QMutex g_mutex;
QFile g_file;
qint64 g_size = 0;
QString g_path;
bool g_console = false;
int g_min_level = 1;
QAtomicInt g_uid(0);

void RestrictPermissions(const QString &path, bool directory);

int toLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return 0;
    case QtInfoMsg: return 1;
    case QtWarningMsg: return 2;
    case QtCriticalMsg: return 3;
    case QtFatalMsg: return 3;
    }
    return 1;
}

char levelLetter(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg: return 'D';
    case QtInfoMsg: return 'I';
    case QtWarningMsg: return 'W';
    case QtCriticalMsg: return 'C';
    case QtFatalMsg: return 'F';
    }
    return 'I';
}

//类别名去掉统一前缀，日志里读作 [net] / [store] / [app]
QString shortCategory(const QMessageLogContext &context)
{
    if (!context.category)
        return QStringLiteral("app");
    const QString full = QString::fromLatin1(context.category);
    if (full.startsWith(QStringLiteral("xc.")))
        return full.mid(3);
    if (full == QStringLiteral("default"))
        return QStringLiteral("app");
    return full;
}

//滚动：丢最旧的，其余依次后移，当前文件清空重开
void rotateLocked()
{
    if (!g_file.isOpen())
        return;
    g_file.close();
    const QString base = g_path;
    QFile::remove(base + QStringLiteral(".%1").arg(kMaxBackups));
    for (int i = kMaxBackups - 1; i >= 1; --i) {
        QFile::rename(base + QStringLiteral(".%1").arg(i),
                      base + QStringLiteral(".%1").arg(i + 1));
    }
    QFile::rename(base, base + QStringLiteral(".1"));
    if (g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        RestrictPermissions(base, false);
        g_size = 0;
    }
}

void writeLocked(const QByteArray &line)
{
    if (g_file.isOpen()) {
        if (g_size + line.size() > kMaxFileSize)
            rotateLocked();
        if (g_file.isOpen()) {
            const qint64 written = g_file.write(line);
            if (written > 0)
                g_size += written;
            g_file.flush(); //进程被强杀时不留半条日志
        }
    }
    if (g_console) {
        std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
        std::fflush(stderr);
    }
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const int level = toLevel(type);
    if (level < g_min_level)
        return;

    const int uid = g_uid.loadAcquire();
    QString line = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));
    line += QStringLiteral(" [%1]").arg(QLatin1Char(levelLetter(type)));
    if (uid > 0)
        line += QStringLiteral(" [uid=%1]").arg(uid);
    line += QStringLiteral(" [%1] ").arg(shortCategory(context));
    line += message;
    line += QLatin1Char('\n');

    QMutexLocker locker(&g_mutex);
    writeLocked(line.toUtf8());
}

int parseLevel(const QString &text, int fallback)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("debug"))
        return kLevelDebug;
    if (value == QStringLiteral("info"))
        return 1;
    if (value == QStringLiteral("warn") || value == QStringLiteral("warning"))
        return 2;
    if (value == QStringLiteral("error"))
        return 3;
    if (value == QStringLiteral("off"))
        return kLevelOff;
    return fallback;
}

bool parseBool(const QString &text, bool fallback)
{
    const QString value = text.trimmed().toLower();
    if (value == QStringLiteral("true") || value == QStringLiteral("1")
        || value == QStringLiteral("yes") || value == QStringLiteral("on"))
        return true;
    if (value == QStringLiteral("false") || value == QStringLiteral("0")
        || value == QStringLiteral("no") || value == QStringLiteral("off"))
        return false;
    return fallback;
}

//日志里有 uid 与消息号，同样只允许本人读；Unix 上收紧到 0700/0600
void RestrictPermissions(const QString &path, bool directory)
{
#if defined(Q_OS_UNIX)
    QFile::Permissions permissions = QFile::ReadOwner | QFile::WriteOwner;
    if (directory)
        permissions |= QFile::ExeOwner;
    QFile::setPermissions(path, permissions);
#else
    Q_UNUSED(path);
    Q_UNUSED(directory);
#endif
}

} // namespace

namespace Log {

void Init()
{
    QSettings settings(QDir(QCoreApplication::applicationDirPath())
                           .filePath(QStringLiteral("config.ini")),
                       QSettings::IniFormat);
    settings.beginGroup(QStringLiteral("Log"));
#ifndef QT_NO_DEBUG
    const int default_level = kLevelDebug;
    const bool default_console = true;
#else
    const int default_level = 1;
    const bool default_console = false;
#endif
    g_min_level = parseLevel(settings.value(QStringLiteral("level")).toString(), default_level);
    g_console = parseBool(settings.value(QStringLiteral("console")).toString(), default_console);
    const bool to_file = parseBool(settings.value(QStringLiteral("file")).toString(), true);
    const QString rules = settings.value(QStringLiteral("rules")).toString().trimmed();
    settings.endGroup();

    if (!rules.isEmpty())
        QLoggingCategory::setFilterRules(rules);

    if (to_file) {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (!root.isEmpty()) {
            const QString dir = QDir(root).filePath(QStringLiteral("logs"));
            if (QDir().mkpath(dir)) {
                RestrictPermissions(dir, true);
                g_path = QDir(dir).filePath(QStringLiteral("client.log"));
                g_file.setFileName(g_path);
                if (g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
                    RestrictPermissions(g_path, false);
                    g_size = g_file.size();
                    if (g_size > kMaxFileSize) {
                        QMutexLocker locker(&g_mutex);
                        rotateLocked();
                    }
                } else {
                    g_path.clear();
                }
            }
        }
    }

    qInstallMessageHandler(messageHandler);
    //偏好设置文件位置随组织名/应用名变化，支持排查时直接看到实际路径
    qCInfo(lcApp, "settings file=%s", QSettings().fileName().toUtf8().constData());
    qCInfo(lcApp, "client start, version=%s qt=%s os=%s log=%s",
           QCoreApplication::applicationVersion().toUtf8().constData(),
           qVersion(), QSysInfo::prettyProductName().toUtf8().constData(),
           g_path.isEmpty() ? "console" : g_path.toUtf8().constData());
}

void Shutdown()
{
    qCInfo(lcApp, "client exit");
    qInstallMessageHandler(nullptr);
    QMutexLocker locker(&g_mutex);
    if (g_file.isOpen()) {
        g_file.flush();
        g_file.close();
    }
}

void SetUser(int uid)
{
    g_uid.storeRelease(uid > 0 ? uid : 0);
}

QString FilePath()
{
    return g_path;
}

} // namespace Log
