#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include "singleton.h"

// 文本与图片消息的组帧、本地记录和资源上传入口；不依赖聊天页面的生命周期。
class ChatService : public Singleton<ChatService>
{
public:
    friend class Singleton<ChatService>;
    bool sendText(int uid, const QString &text);
    //图片先发 1031 落库，收到回执后由 uploadPendingImage 启动 2001 断点上传
    bool sendImage(int uid, const QString &source, int width, int height);
    void uploadPendingImage(const QString &uniqueId, qint64 messageId, int touid, qint64 threadId);
    void discardPendingImage(const QString &uniqueId); //服务端拒绝时丢掉内存里的图片数据
    static QString ImageCachePath(const QString &name); //本机图片缓存，也是上传源文件
    void ensurePrivateChat(int uid);

private:
    ChatService() = default;

    //一次待上传的图片：数据在等待服务端回执期间留在内存，回执后落盘并上传
    struct PendingImage {
        QByteArray data;
        QString name;
        QString md5;
        int uid = 0;
    };

    QHash<QString, PendingImage> _pending_images; //unique_id -> 待上传图片
};

#endif // CHATSERVICE_H
