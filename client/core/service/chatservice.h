#ifndef CHATSERVICE_H
#define CHATSERVICE_H

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include "singleton.h"
#include "userdata.h"
#include "localstore.h"

// 文本、图片、视频与文件消息的组帧、本地记录和资源上传入口；
// 不依赖聊天页面的生命周期，QML 只通过 ChatBridge 查询与操作。
class ChatService : public Singleton<ChatService>
{
public:
    friend class Singleton<ChatService>;
    ~ChatService();
    //单聊与群聊共用同一条发送链路：会话上下文决定写哪张表、通知哪些人
    bool sendText(const ConversationRef &conv, const QString &text,
        const QStringList &mentions = {});
    //图片先发 1031 落库，收到回执后由 uploadPendingImage 启动 1037 断点上传；
    //thumbnail 是气泡用的缩略图，uploadPath 是输入区压缩好的上传文件
    bool sendImage(const ConversationRef &conv, const QString &thumbnail,
        const QString &uploadPath, int width, int height);
    //视频先落一条本地消息，异步取首帧并上传；缩略图就绪后再发 1034，资源随后上传
    bool sendVideo(const ConversationRef &conv, const QString &path, const QString &fileName,
        qint64 fileSize);
    //文件不需要预览，直接落库并在回执后上传
    bool sendFile(const ConversationRef &conv, const QString &path, const QString &fileName,
        qint64 fileSize);
    void uploadPendingImage(const QString &uniqueId, qint64 messageId, const ConversationRef &conv);
    void discardPendingImage(const QString &uniqueId); //服务端拒绝时丢掉待上传记录
    //把会话已读到最新：只在"正在看"的时候调用，同一位置不会重复上报
    void MarkSessionRead(const ConversationRef &conv);
    //撤回自己发的消息：服务端校验时间窗与归属，成功回执后本地换成提示
    void RecallMessage(qint64 messageId);
    //重发一条失败的消息：文本直接重发，图片/视频/文件用本机文件重新走一遍发送流程
    bool RetryMessage(const ConversationRef &conv, const QString &msgId);
    void HandleResourceRsp(const QJsonObject &data); //1035 视频/文件回执，启动资源上传
    //接收方收到 1033：补一条图片消息，本地没有压缩图时发起下载
    void HandleImageNotify(const QJsonObject &data);
    //接收方收到 1036：补视频/文件消息，只自动下载视频首帧，资源本体等用户点击
    void HandleResourceNotify(const QJsonObject &data);
    //历史消息或通知里的图片没有本地文件时排入下载队列，由连接状态与并发上限依次放行
    void DownloadChatImage(const QString &name, int sender, qint64 messageId, qint64 threadId);
    void DownloadVideoThumb(const QString &thumbName, int sender, qint64 messageId,
        qint64 threadId); //历史视频的预览资源
    void DownloadResource(qint64 messageId); //接收方点击视频/文件卡片
    //暂停/继续或重试；服务端还没给消息 ID 时用 uniqueId 兜底定位
    void ToggleResourceTransfer(qint64 messageId, const QString &uniqueId = {});
    void RetryImageDownloads(); //资源连接就绪后补发等待中的图片与视频首帧
    //按类型分目录：图片 images、视频 videos、文件 files，资源名都是 <md5>.<ext>
    static QString ResourcePath(ChatMsgType type, const QString &name);
    static QString ImageCachePath(const QString &name);
    static QString ImageThumbPath(const QString &name); //气泡缩略图缓存
    static QString VideoThumbPath(const QString &name); //视频首帧资源缓存
    //有完整图片时按需生成缩略图，成功返回缩略图路径并带回像素尺寸
    static QString EnsureImageThumbnail(const QString &name, QSize *pixelSize = nullptr);
    //查看器、打开与另存为使用：返回本机完整资源路径，缺失时返回空
    static QString LocalPathForMessage(qint64 messageId);
    //视频/文件的 content 是 JSON 元数据，历史同步与通知共用这套解析
    static bool ParseResourceMeta(const QString &content, QString *fileName, qint64 *size,
        QString *thumb, QString *resource = nullptr);
    static std::shared_ptr<TextChatData> BuildResourceMessage(int from, int to, qint64 messageId,
        qint64 threadId, int msgType, const QString &content, const QString &resourceName,
        ChatMessageStatus status);
    void ensurePrivateChat(int uid);

    //打开会话：从本地库取最近一页装进内存窗口，返回是否还有更早的消息。
    //窗口只在"正在看"的会话上存在，切走不保留，长期使用内存不随历史增长
    bool OpenWindow(qint64 threadId);
    //上滑翻页：再取更早一页插到窗口头部，返回是否还有更早
    bool LoadOlder(qint64 threadId);
    //把库里的一行还原成内存消息：补全本地资源路径，缺文件时按需排入下载队列。
    //历史同步与本地翻页共用这一条路径，避免两套解析逻辑慢慢跑偏
    std::shared_ptr<TextChatData> MessageFromStored(const StoredMessage &row, int selfUid);
    //清除本账号已下载的图片、视频与文件；消息记录本身不动
    void ClearMediaCache();

private:
    ChatService();

    //一次待上传的图片：回执到达前只记本机缓存路径，不再把图片数据留在内存
    struct PendingImage {
        QString path;
        QString name;
        QString md5;
        ConversationRef conv;
    };
    QHash<QString, PendingImage> _pending_images; //unique_id -> 待上传图片
    //thread_id -> 已经上报到的 message_id；避免每次切回会话都重复发 1052
    QHash<qint64, qint64> _read_reported;
    //发出后等回执的兜底：超过时限仍是"发送中"就标失败，不等用户发现
    void StartSendTimeout(const ConversationRef &conv, const QString &msgId);

    //视频/文件在等 1035 回执：回执到达后才把资源本体交给 FileService
    struct PendingResource {
        ChatMsgType type = ChatMsgType::File;
        ConversationRef conv;
        QString path;
        QString name;
        QString md5;
        QString file_name;
        qint64 file_size = 0;
        QString thumb_name;
        QString unique_id;
    };
    QHash<QString, PendingResource> _pending_resources; //unique_id -> 待上传资源

    //视频首帧还在生成/上传时先把消息准备好，缩略图完成后才发 1034
    struct PendingVideo {
        ConversationRef conv;
        QString path;
        QString name;
        QString md5;
        QString file_name;
        qint64 file_size = 0;
    };
    QHash<QString, PendingVideo> _pending_videos; //unique_id -> 准备中的视频
    QHash<QString, QString> _video_by_thumb; //首帧资源名 -> unique_id

    //一次接收方下载：完成后按资源名找到所有引用它的消息
    struct ChatDownload {
        QString name;
        QString path; //半截文件位置，断线/换账号时要删掉
        int sender = 0;
        qint64 message_id = 0;
        qint64 thread_id = 0;
        ChatMsgType type = ChatMsgType::Image;
        bool thumb = false; //视频首帧：只更新预览，不改变视频下载状态
        bool paused = false;
        qint64 expected_size = 0; //视频/文件用于识别上次中断留下的半截文件
    };
    //登录会一次拉到很多历史图片，排队后最多同时下两个，避免占满资源连接、进度条一起乱跳
    static constexpr int MaxConcurrentImageDownloads = 2;
    QHash<QString, ChatDownload> _chat_downloads; //正在下载的资源
    QList<ChatDownload> _download_queue; //等待下载的资源，实时通知插到队首
    void enqueueDownload(const ChatDownload &task, bool urgent);
    void pumpDownloads(); //按并发上限从队列取任务
    int activeDownloadCount() const; //暂停的任务不占并发位

    void startVideoMessage(const QString &uniqueId, const QImage &image);
    void sendVideoRequest(const QString &uniqueId, const QString &thumbName);
    void sendResourceRequest(const PendingResource &resource);
    void ensureVideoThumb(const QString &thumbName, int sender, qint64 messageId, qint64 threadId);
    void applyImageDownloaded(const ChatDownload &task, const QString &localPath);
    //按本地文件是否齐全补全图片/视频/文件的显示状态，不改变消息本身的事实字段
    void hydrateLocal(const std::shared_ptr<TextChatData> &message);
};

#endif // CHATSERVICE_H
