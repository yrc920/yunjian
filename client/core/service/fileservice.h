#ifndef FILESERVICE_H
#define FILESERVICE_H

#include <QObject>
#include <QMap>
#include <QString>
#include "singleton.h"
#include "global.h"

/******************************************************************************
 * @file       fileservice.h
 * @brief      头像同步编排：建立资源连接、断点上传/下载、暂停与进度
 *
 * FileTcpMgr 只负责协议与分片读写，这里负责业务状态：
 *   · 头像裁剪保存后上传，失败不影响本机头像
 *   · 登录后拉取本机/好友缺失的头像并写入本地缓存
 *   · 暂停时停在当前分片，继续前先向服务器同步进度
 *****************************************************************************/

class FileService : public QObject, public Singleton<FileService>
{
    Q_OBJECT

public:
    friend class Singleton<FileService>;
    ~FileService();

    void ConnectToResource(const ServerInfo &info);
    void CloseResource();
    void OnSessionReady(); //聊天登录完成，可以拉取缺失头像

    void UploadAvatar(const QString &path);
    void SetUploadPaused(bool paused);
    void ToggleUploadPaused();

    bool IsUploading() const { return _uploading; }
    bool IsPaused() const { return _paused; }
    int Progress() const;
    QString Message() const { return _message; }

signals:
    void uploadStateChanged();

private:
    FileService();

    void TryStartResourceSync();
    void RequestResource(int ownerUid, const QString &iconName, const QString &targetPath);
    void CancelOwnAvatarDownload();
    void HandleUploadProgress(const QString &name, qint64 trans, qint64 total);
    void HandleUploadFinished(const QString &name);
    void HandleUploadFailed(const QString &name, int error);
    void HandleDownloadFinished(const QString &name, const QString &path);
    void HandleDownloadFailed(const QString &name, int error);

    bool _connected = false;
    bool _sessionReady = false;
    bool _resourceConfigured = false;
    bool _uploading = false;
    bool _paused = false;
    QString _uploadMd5;
    QString _uploadName;
    QString _message;
    qint64 _trans_size = 0;
    qint64 _total_size = 0;
    QMap<QString, int> _download_owners; //资源名 -> 头像属于谁
};

#endif // FILESERVICE_H
