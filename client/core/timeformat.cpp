#include "timeformat.h"

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace {
//周一开头，与 QDate::dayOfWeek() 的 1..7 对齐
const char* const kWeekNames[] = {"周一", "周二", "周三", "周四", "周五", "周六", "周日"};

//相对日：今天 / 昨天 / 前天 / 一周内按周几，其余按日期。
//跨年但只差一两天时仍然显示"昨天/前天"，因为人对间隔的感知与年份无关
QString relativeDay(const QDate& date, bool withYear)
{
    const QDate today = QDate::currentDate();
    const int days = static_cast<int>(date.daysTo(today));

    if (days == 0)
        return {};
    if (days == 1)
        return QStringLiteral("昨天");
    if (days == 2)
        return QStringLiteral("前天");
    if (days > 2 && days < 7) {
        const int index = date.dayOfWeek() - 1;
        return QString::fromUtf8(kWeekNames[index]);
    }
    if (date.year() == today.year())
        return QStringLiteral("%1月%2日").arg(date.month()).arg(date.day());
    return withYear ? QStringLiteral("%1年%2月%3日").arg(date.year()).arg(date.month()).arg(date.day())
                    : QStringLiteral("%1/%2/%3").arg(date.year()).arg(date.month()).arg(date.day());
}

//无效时间返回空串，界面据此不渲染时间，而不是显示 1970 年
bool resolve(qint64 secs, QDateTime& local)
{
    if (secs <= 0)
        return false;
    local = QDateTime::fromSecsSinceEpoch(secs);
    return local.isValid();
}
} // namespace

namespace TimeFormat {

QString listTime(qint64 secs)
{
    QDateTime local;
    if (!resolve(secs, local))
        return {};

    const QString day = relativeDay(local.date(), false);
    if (day.isEmpty())
        return local.time().toString(QStringLiteral("HH:mm"));
    return day;
}

QString messageTime(qint64 secs)
{
    QDateTime local;
    if (!resolve(secs, local))
        return {};

    const QString clock = local.time().toString(QStringLiteral("HH:mm"));
    const QString day = relativeDay(local.date(), true);
    if (day.isEmpty())
        return clock;
    return day + QStringLiteral(" ") + clock;
}

} // namespace TimeFormat
