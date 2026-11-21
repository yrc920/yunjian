#include "autologinstore.h"

#include <QDateTime>
#include <QSettings>

namespace {

// 与服务端的令牌有效期保持一致；客户端只做提前拦截，真正的判断仍由服务端做
constexpr qint64 kTokenLifeSeconds = 7 * 24 * 60 * 60;
constexpr auto kGroup = "AutoLogin";

QString key(const char *name)
{
    return QString::fromLatin1(name);
}

} // namespace

bool AutoLoginStore::Remembered()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    return settings.value(key("remember"), true).toBool();
}

void AutoLoginStore::SetRemembered(bool on)
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    settings.setValue(key("remember"), on);
}

bool AutoLoginStore::Load(Record &out)
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    if (!settings.value(key("remember"), true).toBool())
        return false;

    Record record;
    record.Email = settings.value(key("email")).toString();
    record.Info.Uid = settings.value(key("uid")).toInt();
    record.Info.Host = settings.value(key("host")).toString();
    record.Info.Port = settings.value(key("port")).toString();
    record.Info.Token = settings.value(key("token")).toString();
    record.Info.ResHost = settings.value(key("resHost")).toString();
    record.Info.ResPort = settings.value(key("resPort")).toString();
    record.SavedAt = settings.value(key("savedAt")).toLongLong();

    if (record.Email.isEmpty() || record.Info.Token.isEmpty() || record.Info.Uid <= 0
        || record.Info.Host.isEmpty() || record.Info.Port.isEmpty()) {
        return false;
    }

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (record.SavedAt <= 0 || now - record.SavedAt >= kTokenLifeSeconds)
        return false;

    out = record;
    return true;
}

void AutoLoginStore::Save(const Record &record)
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    settings.setValue(key("email"), record.Email);
    settings.setValue(key("uid"), record.Info.Uid);
    settings.setValue(key("host"), record.Info.Host);
    settings.setValue(key("port"), record.Info.Port);
    settings.setValue(key("token"), record.Info.Token);
    settings.setValue(key("resHost"), record.Info.ResHost);
    settings.setValue(key("resPort"), record.Info.ResPort);
    settings.setValue(key("savedAt"), record.SavedAt);
    settings.setValue(key("remember"), true);
}

void AutoLoginStore::Clear()
{
    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kGroup));
    for (const char *name : {"email", "uid", "host", "port", "token", "resHost", "resPort", "savedAt"})
        settings.remove(key(name));
}
