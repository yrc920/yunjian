#ifndef LOG_H
#define LOG_H

#include <QLoggingCategory>
#include <QString>

/******************************************************************************
 *
 * @file       log.h
 * @brief      客户端日志：分级、分类、落盘与轮转
 *
 * 三端统一用 Qt 的日志类别，业务代码只写 qCDebug(lcNet) << "..."，
 * 不直接碰文件、不加时间戳前缀。输出格式、级别与落盘位置由 Log::Init 决定。
 *
 * 日志目录：<AppData>/YunJian/logs/client.log，单文件 8 MiB，保留 3 个历史文件；
 * 控制台输出只在 [Log] console=true 或 Debug 构建时打开。
 *
 *****************************************************************************/

// 网络：连接、TLS、帧收发、重连
Q_DECLARE_LOGGING_CATEGORY(lcNet)
// 本地缓存库：打开、迁移、SQL 失败
Q_DECLARE_LOGGING_CATEGORY(lcStore)
// 业务与会话：登录、好友、消息、群、资源上传下载
Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace Log {

// 读取 config.ini 的 [Log] 段并安装消息处理器；main 里紧随 setApplicationName 调用
void Init();
// 摘掉处理器并落盘；main 退出前调用
void Shutdown();

// 登录后写入 uid，之后每行日志都带上，排查时不必再对照登录时间
void SetUser(int uid);

QString FilePath();

} // namespace Log

#endif // LOG_H
