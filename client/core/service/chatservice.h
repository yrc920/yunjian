#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include "singleton.h"

// 文本与图片消息的组帧、本地记录和资源上传入口；不依赖聊天页面的生命周期。
class ChatService : public Singleton<ChatService>
{
public:
    friend class Singleton<ChatService>;
    bool sendText(int uid, const QString &text);
    //图片先发 1031 落库，收到回执后由 uploadPendingImage 启动 2001 断点上传；
    //thumbnail 是气泡用的缩略图，uploadPath 是输入区压缩好的上传文件
    bool sendImage(int uid, const QString &thumbnail, const QString &uploadPath,
        int width, int height);
    void uploadPendingImage(const QString &uniqueId, qint64 messageId, int touid, qint64 threadId);
    void discardPendingImage(const QString &uniqueId); //服务端拒绝时丢掉待上传记录
    //接收方收到 1033：补一条图片消息，本地没有压缩图时发起下载
    void HandleImageNotify(const QJsonObject &data);
    //历史消息或通知里的图片没有本地文件时排入下载队列，由连接状态与并发上限依次放行
    void DownloadChatImage(const QString &name, int sender, qint64 messageId, qint64 threadId);
    void RetryImageDownloads(); //资源连接就绪后补发等待中的图片下载
    //本机压缩图，与服务器保存的是同一份（长边 1600），点开大图直接读它；
    //气泡和输入区只用 thumbs 下的缩略图，两者分开存放
    static QString ImageCachePath(const QString &name);
    static QString ImageThumbPath(const QString &name); //气泡缩略图缓存
    //有完整图片时按需生成缩略图，成功返回缩略图路径并带回像素尺寸
    static QString EnsureImageThumbnail(const QString &name, QSize *pixelSize = nullptr);
    void ensurePrivateChat(int uid);

private:
    ChatService();

    //一次待上传的图片：回执到达前只记本机缓存路径，不再把图片数据留在内存
    struct PendingImage {
        QString path;
        QString name;
        QString md5;
        int uid = 0;
    };

    QHash<QString, PendingImage> _pending_images; //unique_id -> 待上传图片

    //一次接收方图片下载：完成后按资源名找到所有引用它的消息
    struct ChatDownload {
        QString name;
        QString path; //半截文件位置，断线/换账号时要删掉
        int sender = 0;
        qint64 message_id = 0;
        qint64 thread_id = 0;
    };
    //登录会一次拉到很多历史图片，排队后最多同时下两个，避免占满资源连接、进度条一起乱跳
    static constexpr int MaxConcurrentImageDownloads = 2;
    QHash<QString, ChatDownload> _chat_downloads; //正在下载的资源
    QList<ChatDownload> _download_queue; //等待下载的资源，实时通知插到队首
    void enqueueImageDownload(const ChatDownload &task, bool urgent);
    void pumpImageDownloads(); //按并发上限从队列取任务
};

#endif // CHATSERVICE_H
