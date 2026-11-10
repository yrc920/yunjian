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

    void ConnectToResource(const ServerInfo &info); //建立资源服务器连接
    void CloseResource(); //退出登录/换账号时断开，并清理下载归属
    void OnSessionReady(); //聊天登录完成，可以拉取缺失头像

    void UploadAvatar(const QString &path); //裁剪器保存后的PNG路径
    void SetUploadPaused(bool paused); //暂停/继续，继续时会先同步服务器进度
    void ToggleUploadPaused(); //QML按钮用

    bool IsUploading() const { return _uploading; } //是否显示进度条
    bool IsPaused() const { return _paused; } //按钮文案用
    int Progress() const; //0..100
    QString Message() const { return _message; } //个人资料页提示

signals:
    //所有状态变化走同一个信号，QML 只绑四个只读属性，不直接认识上传细节
    void uploadStateChanged();

private:
    FileService();

    void TryStartResourceSync(); //连接与登录都完成后补拉头像
    void CacheContactIcon(int uid, const QString &iconName); //查缓存或发起下载
    void RequestResource(int ownerUid, const QString &iconName, const QString &targetPath); //建下载状态
    void AbortDownloads(); //断线/换账号时清掉在途下载与半截文件
    void CancelOwnAvatarDownload(); //上传前取消本机旧头像下载
    void HandleUploadProgress(const QString &name, qint64 trans, qint64 total); //更新进度
    void HandleUploadFinished(const QString &name); //收尾
    void HandleUploadFailed(const QString &name, int error); //结束并提示
    void HandleDownloadFinished(const QString &name, const QString &path); //写回好友/申请/本机
    void HandleDownloadFailed(const QString &name, int error); //删半截缓存

    bool _connected = false; //资源服务器TCP是否已连上
    bool _sessionReady = false; //聊天登录是否已完成，两个条件都满足才开始拉头像
    bool _resourceConfigured = false; //服务端是否下发了res_host/res_port
    bool _uploading = false; //是否有头像正在同步（暂停时仍为true）
    bool _paused = false; //用户是否点了暂停
    QString _uploadMd5; //当前上传的资源名，暂停续传和失败清理都要用
    QString _uploadName; //md5.png，显示提示与日志用
    QString _message; //个人资料页展示的同步状态
    qint64 _trans_size = 0; //服务器已确认落盘的字节数
    qint64 _total_size = 0; //本次头像总大小
    QMap<QString, int> _download_owners; //资源名 -> 头像属于谁，回包后更新对应联系人
};

#endif // FILESERVICE_H
