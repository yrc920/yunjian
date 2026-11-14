#ifndef TIMEFORMAT_H
#define TIMEFORMAT_H

#include <QString>

// 时间展示规则集中在这里：会话列表位置窄、要短；消息时间条可以完整一些。
// 两者共用同一套"相对日"判断，避免同一个时间在列表和聊天里显示成两个样子。
namespace TimeFormat {

// 聊天内两条消息间隔超过这个秒数才插一条时间条（与常见 IM 的 5 分钟一致）
constexpr qint64 kSeparatorSeconds = 5 * 60;

// 会话列表右上角：今天 14:32 / 昨天 / 前天 / 周三 / 3月5日 / 2025/3/5
QString listTime(qint64 secs);

// 消息时间条：今天 14:32 / 昨天 14:32 / 前天 14:32 / 周三 14:32 / 3月5日 14:32 / 2025年3月5日 14:32
QString messageTime(qint64 secs);

} // namespace TimeFormat

#endif // TIMEFORMAT_H
