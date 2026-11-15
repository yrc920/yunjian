#include "chatservice.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "fileservice.h"
#include "groupservice.h"
#include "filetcpmgr.h"
#include "sessionservice.h"
#include "tcpmgr.h"
#include "usermgr.h"
#include "videothumbnail.h"

namespace {
constexpr int ThumbMaxWidth = 480; //与 ComposerBridge 的图片缩略图尺寸保持一致
constexpr int ThumbMaxHeight = 360;
constexpr int ThumbQuality = 86;
constexpr int VideoThumbMaxWidth = 480; //视频首帧按 16:9 准备，卡片里再等比缩放
constexpr int VideoThumbMaxHeight = 270;
constexpr int VideoThumbQuality = 82;
constexpr int MaxSuffixLength = 12;

//资源名后缀只取安全字符，避免拼出带路径的文件名
QString resourceSuffix(const QString &path)
{
    QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty() || suffix.size() > MaxSuffixLength)
        return QStringLiteral("bin");
    for (const QChar ch : suffix) {
        if (!ch.isLetterOrNumber())
            return QStringLiteral("bin");
    }
    return suffix;
}

//视频/文件按上传者的原文件名保存：去掉路径分隔符与系统保留字符，
//并把 UTF-8 长度压到 80 字节以内，给重名后缀和扩展名留空间
QString safeBaseName(const QString &original)
{
    QString base = QFileInfo(original).completeBaseName().trimmed();
    if (base.isEmpty())
        base = QStringLiteral("文件");

    QString cleaned;
    cleaned.reserve(base.size());
    for (const QChar ch : base) {
        if (ch.unicode() < 0x20 || QStringLiteral("/\\:*?\"<>|").contains(ch))
            cleaned += QLatin1Char('_');
        else
            cleaned += ch;
    }
    while (cleaned.startsWith(QLatin1Char('.')))
        cleaned.remove(0, 1);
    while (!cleaned.isEmpty() && cleaned.toUtf8().size() > 80)
        cleaned.chop(1);
    return cleaned.isEmpty() ? QStringLiteral("文件") : cleaned;
}

QString fileMd5(const QString &path);

//重名时追加 (2)、(3)：内容 md5 相同的同名文件视为同一份，重复发送不再堆副本
QString uniqueResourceName(ChatMsgType type, const QString &sourcePath,
                           const QString &original, qint64 sourceSize,
                           const QString &contentMd5)
{
    const QString base = safeBaseName(original);
    const QString suffix = resourceSuffix(sourcePath);
    for (int index = 1; index < 1000; ++index) {
        const QString name = index == 1
            ? base + QLatin1Char('.') + suffix
            : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
        const QString target = ChatService::ResourcePath(type, name);
        if (target.isEmpty() || !QFileInfo::exists(target))
            return name;
        if (QFileInfo(target).size() == sourceSize && fileMd5(target) == contentMd5)
            return name; //同一份内容沿用已有资源名
    }
    return base + QLatin1Char('.') + suffix;
}

//大视频不能整体读进内存，按流计算 md5
QString fileMd5(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Md5);
    if (!hash.addData(&file))
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

//视频首帧统一存 JPEG；透明通道对视频帧没有意义
QByteArray encodeVideoThumb(const QImage &source)
{
    QImage image = source;
    if (image.width() > VideoThumbMaxWidth || image.height() > VideoThumbMaxHeight)
        image = image.scaled(VideoThumbMaxWidth, VideoThumbMaxHeight, Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly))
        return {};
    return image.save(&buffer, "JPG", VideoThumbQuality) ? bytes : QByteArray();
}
}

ChatService::~ChatService() = default;

ChatService::ChatService()
{
    //资源上传的进度和结果由 FileService 汇总，这里按资源名更新对应消息
    auto *file = FileService::GetInstance().get();
    QObject::connect(file, &FileService::chatResourceProgress, file,
            [this](const QString &name, qint64 trans, qint64 total) {
        if (_video_by_thumb.contains(name))
            return; //首帧资源只用于预览，不占消息进度
        UserMgr::GetInstance()->UpdateResourceUploadProgress(name, trans, total);
    });
    QObject::connect(file, &FileService::chatResourceFinished, file, [this](const QString &name) {
        auto thumb = _video_by_thumb.find(name);
        if (thumb != _video_by_thumb.end()) {
            const QString uniqueId = thumb.value();
            _video_by_thumb.erase(thumb);
            sendVideoRequest(uniqueId, name); //首帧已落盘，可以发 1034 了
            return;
        }
        UserMgr::GetInstance()->FinishResourceUpload(name);
    });
    QObject::connect(file, &FileService::chatResourceFailed, file,
            [this](const QString &name, int) {
        auto thumb = _video_by_thumb.find(name);
        if (thumb != _video_by_thumb.end()) {
            const QString uniqueId = thumb.value();
            _video_by_thumb.erase(thumb);
            auto pending = _pending_videos.constFind(uniqueId);
            if (pending != _pending_videos.constEnd()) {
                auto message = UserMgr::GetInstance()->FindMessageByUniqueId(uniqueId);
                if (message)
                    message->_thumb_name.clear(); //首帧没上传成功，通知里不要带无效资源名
            }
            sendVideoRequest(uniqueId, QString());
            return;
        }
        UserMgr::GetInstance()->FailResourceUpload(name);
    });

    //下载结果由资源网络线程发出，用 qApp 做上下文排队回主线程再改消息数据
    auto *file_tcp = FileTcpMgr::GetInstance().get();
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_progress, qApp,
            [this](const QString &name, qint64 current, qint64 total) {
        const auto it = _chat_downloads.constFind(name);
        if (it == _chat_downloads.constEnd() || it->thumb)
            return; //头像与视频首帧不动消息进度
        UserMgr::GetInstance()->UpdateResourceDownloadProgress(name, current, total);
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_finished, qApp,
            [this](const QString &name, const QString &path) {
        const auto it = _chat_downloads.find(name);
        if (it != _chat_downloads.end()) {
            const ChatDownload task = it.value();
            _chat_downloads.erase(it);
            if (task.thumb) {
                UserMgr::GetInstance()->SetVideoThumb(name, path, QImageReader(path).size());
            }
            else if (task.type == ChatMsgType::Image) {
                QSize pixel;
                const QString thumb = EnsureImageThumbnail(name, &pixel);
                if (thumb.isEmpty())
                    UserMgr::GetInstance()->FailResourceDownload(name);
                else
                    UserMgr::GetInstance()->FinishResourceDownload(name,
                        ImageCachePath(name), thumb, pixel);
            }
            else {
                UserMgr::GetInstance()->FinishResourceDownload(name, path, {}, {});
            }
        }
        pumpDownloads(); //腾出一个并发位，继续下一张
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_failed, qApp,
            [this](const QString &name, int) {
        const auto it = _chat_downloads.find(name);
        if (it != _chat_downloads.end()) {
            const ChatDownload task = it.value();
            _chat_downloads.erase(it);
            if (!task.path.isEmpty())
                QFile::remove(task.path); //半截文件不能留，重试时从第一片覆盖
            if (!task.thumb)
                UserMgr::GetInstance()->FailResourceDownload(name);
        }
        pumpDownloads();
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_connection_closed, qApp, [this] {
        //资源连接断开后这些下载不会再有回包，清掉记录并删掉半截文件
        for (auto it = _chat_downloads.constBegin(); it != _chat_downloads.constEnd(); ++it) {
            UserMgr::GetInstance()->RemoveDownloadFile(it.key());
            if (!it.value().path.isEmpty())
                QFile::remove(it.value().path);
            if (!it.value().thumb)
                UserMgr::GetInstance()->FailResourceDownload(it.key());
        }
        _chat_downloads.clear();
        _download_queue.clear(); //重连后由 RetryImageDownloads 按消息重新排队
    });
    //登录时图片通知可能先于资源连接到达，连接就绪后统一补发一次；
    //排到事件循环末尾，确保 FileService 已经把 _connected 置为 true
    QObject::connect(file_tcp, &FileTcpMgr::sig_con_success, qApp, [this](bool success) {
        if (success)
            QTimer::singleShot(0, qApp, [this] { RetryImageDownloads(); });
    });
    //换账号前把上一个账号的待上传图、在途下载和排队任务一起作废，
    //否则新账号可能继续按旧资源名下载，文件会落到新账号目录里
    QObject::connect(UserMgr::GetInstance().get(), &UserMgr::SessionCleared,
                     UserMgr::GetInstance().get(), [this] {
        _pending_images.clear();
        _pending_resources.clear();
        _pending_videos.clear();
        _video_by_thumb.clear();
        _read_reported.clear(); //换账号后已读位置要重新上报
        for (auto it = _chat_downloads.constBegin(); it != _chat_downloads.constEnd(); ++it) {
            UserMgr::GetInstance()->RemoveDownloadFile(it.key());
            if (!it.value().path.isEmpty())
                QFile::remove(it.value().path);
        }
        _chat_downloads.clear();
        _download_queue.clear();
    });
}

//会话是否可发：单聊要求好友仍在，群聊要求本地有群对象。
//单聊顺带触发 1025 建会话，群聊的 thread_id 在入群时就已确定
bool ensureConversation(const ConversationRef &conv)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady())
        return false;
    if (conv._kind == ChatThreadKind::Group)
        return conv._thread_id > 0 && mgr->GetGroup(conv._thread_id) != nullptr;
    if (!mgr->GetFriendById(conv._peer_uid))
        return false;
    ChatService::GetInstance()->ensurePrivateChat(conv._peer_uid);
    return true;
}

//发送帧的公共字段：群聊的 touid 固定 0，thread_id 与 thread_type 决定服务端写哪张表
void fillConversationFrame(QJsonObject &frame, const ConversationRef &conv)
{
    frame["fromuid"] = UserMgr::GetInstance()->GetUid();
    frame["touid"] = conv._peer_uid;
    frame["thread_id"] = QJsonValue::fromVariant(conv._thread_id);
    frame["thread_type"] = static_cast<int>(conv._kind);
}

// 打开会话：只有登录就绪、会话不存在且没有在途请求时才发 1025，
// 避免连续点同一个好友时重复建 thread
void ChatService::ensurePrivateChat(int uid)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady() || uid <= 0 || mgr->ThreadId(uid) > 0
        || mgr->HasPendingPrivateChat(uid))
        return;
    mgr->MarkPrivateChatPending(uid, true);
    QJsonObject request{{"fromuid", mgr->GetUid()}, {"touid", uid}};
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_CREATE_PRIVATE_CHAT_REQ,
        QJsonDocument(request).toJson(QJsonDocument::Compact));
}

// 发送文本：先落一条本地待确认消息，再按服务端帧上限拆分发送。
// 返回 true 只代表已交给网络层，不代表对方收到
bool ChatService::sendText(const ConversationRef &conv, const QString &text,
                           const QStringList &mentions)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!ensureConversation(conv))
        return false;
    if (text.isEmpty())
        return true;

    const int from = mgr->GetUid();
    qsizetype offset = 0;
    while (offset < text.size()) {
        const QString msgId = QUuid::createUuid().toString();
        QJsonObject frame;
        fillConversationFrame(frame, conv);
        //@ 名单只对群聊有意义；长文本拆成多帧时每帧都带，服务端按消息各存一份
        QJsonArray mention_array;
        for (const QString &uid : mentions) {
            bool ok = false;
            const int value = uid.toInt(&ok);
            if (ok && value > 0)
                mention_array.append(value);
        }
        if (!mention_array.isEmpty())
            frame["mentions"] = mention_array;
        QByteArray payload;
        QString content;

        // 服务端接收上限为 2048 字节。按 JSON 编码后的字节数分帧，
        // 预留转发时的 error 字段与格式化开销；UTF-16 代理对不可拆开。
        while (offset < text.size()) {
            const qsizetype count = text.at(offset).isHighSurrogate()
                && offset + 1 < text.size() && text.at(offset + 1).isLowSurrogate() ? 2 : 1;
            const QString next = content + text.mid(offset, count);
            frame["text_array"] = QJsonArray{QJsonObject{{"msgid", msgId}, {"content", next}}};
            const QByteArray encoded = QJsonDocument(frame).toJson(QJsonDocument::Compact);
            if (encoded.size() > 1800)
                break;
            content = next;
            payload = encoded;
            offset += count;
        }

        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_TEXT_CHAT_MSG_REQ, payload);
        auto pending = std::make_shared<TextChatData>(msgId, content, from, conv._peer_uid, 0,
            conv._thread_id, ChatMessageStatus::Sending);
        //先按本机时间占位，回执带回服务端时间后原地覆盖，避免发送中这一行没有时间
        pending->_time = QDateTime::currentSecsSinceEpoch();
        mgr->AppendChatMsgs(conv, {pending});
        StartSendTimeout(conv, msgId);
    }
    return true;
}

QString ChatService::ResourcePath(ChatMsgType type, const QString &name)
{
    return UserMgr::ResourcePath(type, name);
}

//把会话已读到最新。只取已经有服务端消息号的消息，本地还没回执的占位不算；
//同一 thread 上报到同一个位置时直接跳过，避免切换会话反复发请求。
//群聊由服务端推进成员表的已读游标，单聊仍按 is_read 标记
void ChatService::MarkSessionRead(const ConversationRef &conv)
{
    if (conv._thread_id <= 0)
        return;
    const qint64 threadId = conv._thread_id;
    const qint64 lastId = UserMgr::GetInstance()->LastMessageId(conv);
    if (lastId <= 0 || _read_reported.value(threadId, 0) >= lastId)
        return;
    _read_reported[threadId] = lastId;

    QJsonObject frame;
    frame["thread_id"] = threadId;
    frame["message_id"] = lastId;
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_MARK_READ_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

//撤回：只发消息号，会话与对方由服务端按库里记录确定
void ChatService::RecallMessage(qint64 messageId)
{
    if (messageId <= 0)
        return;
    QJsonObject frame;
    frame["message_id"] = messageId;
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_RECALL_MSG_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

//发出后等回执的兜底：网络半死（连着但不通）时心跳不一定立刻断，
//用 10 秒超时把消息标失败，比让用户盯着"发送中"更好
void ChatService::StartSendTimeout(const ConversationRef &conv, const QString &msgId)
{
    QTimer::singleShot(10000, [conv, msgId] {
        UserMgr::GetInstance()->MarkMessageFailed(conv, msgId);
    });
}

//重发失败消息：先取出内容与本地文件，删掉旧记录，再走一遍正常的发送流程。
//文本直接复用；图片用缩略图重传；视频/文件必须有本机原文件，否则只能重新选择
bool ChatService::RetryMessage(const ConversationRef &conv, const QString &msgId)
{
    auto *mgr = UserMgr::GetInstance().get();
    //按客户端 msgid 全局查找：会话还没拿到 thread_id 时也能定位到这条占位消息
    auto target = mgr->FindMessageByUniqueId(msgId);
    if (!target || target->_status != ChatMessageStatus::Failed || target->_recalled)
        return false;

    const ChatMsgType type = target->_msg_type;
    const QString text = target->_msg_content;
    const QString thumb = target->_imageSource;
    const QString local = target->_local_path;
    const QString fileName = target->_file_name;
    const qint64 fileSize = target->_file_size;
    const int width = target->_imageWidth;
    const int height = target->_imageHeight;

    if (type == ChatMsgType::Text) {
        mgr->RemoveChatMessage(conv, msgId);
        return sendText(conv, text);
    }

    if (type == ChatMsgType::Image) {
        QString path = thumb;
        if (path.startsWith(QStringLiteral("file://")))
            path = QUrl(path).toLocalFile();
        if (path.startsWith(QStringLiteral("data:")) || !QFileInfo::exists(path)) {
            return false; //调用方据此提示"原文件已不在本机"
        }
        mgr->RemoveChatMessage(conv, msgId);
        return sendImage(conv, thumb, path, width, height);
    }

    if (local.isEmpty() || !QFileInfo::exists(local))
        return false;
    mgr->RemoveChatMessage(conv, msgId);
    if (type == ChatMsgType::Video)
        return sendVideo(conv, local, fileName, fileSize);
    return sendFile(conv, local, fileName, fileSize);
}

QString ChatService::ImageCachePath(const QString &name)
{
    return UserMgr::ChatImagePath(name); //账号目录由 UserMgr 统一维护
}

QString ChatService::ImageThumbPath(const QString &name)
{
    return UserMgr::ChatImageThumbPath(name);
}

QString ChatService::VideoThumbPath(const QString &name)
{
    return UserMgr::ResourceThumbPath(ChatMsgType::Video, name);
}

QString ChatService::EnsureImageThumbnail(const QString &name, QSize *pixelSize)
{
    const QString full = ImageCachePath(name);
    const QString thumb = ImageThumbPath(name);
    if (full.isEmpty() || thumb.isEmpty() || !QFileInfo::exists(full))
        return {};

    //缩略图比原图新且能读出尺寸时直接复用，避免每次登录都重新解码大图
    if (QFileInfo::exists(thumb)
        && QFileInfo(thumb).lastModified() >= QFileInfo(full).lastModified()) {
        const QSize cached = QImageReader(thumb).size();
        if (cached.isValid()) {
            if (pixelSize)
                *pixelSize = cached;
            return thumb;
        }
    }

    QImageReader reader(full);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull())
        return {};
    if (image.width() > ThumbMaxWidth || image.height() > ThumbMaxHeight)
        image = image.scaled(ThumbMaxWidth, ThumbMaxHeight, Qt::KeepAspectRatio,
            Qt::SmoothTransformation);

    //缩略图统一存 JPEG：历史消息数量多时，解码开销和磁盘占用都更小
    if (image.hasAlphaChannel()) {
        QImage canvas(image.size(), QImage::Format_RGB32);
        canvas.fill(Qt::white);
        QPainter painter(&canvas);
        painter.drawImage(0, 0, image);
        image = canvas;
    }
    QDir().mkpath(QFileInfo(thumb).absolutePath());
    if (!image.save(thumb, "JPG", ThumbQuality))
        return {};
    if (pixelSize)
        *pixelSize = image.size();
    return thumb;
}

// 图片消息：先插入本地消息并发 1031，服务端落库后回执，再上传资源。
// 返回 true 只代表消息已交给网络层
bool ChatService::sendImage(const ConversationRef &conv, const QString &thumbnail,
    const QString &uploadPath, int width, int height)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!ensureConversation(conv))
        return false;

    //上传源由输入区压缩好，这里只读一次算 md5，不再经过 QML 传递大块数据
    const QFileInfo upload_info(uploadPath);
    if (!upload_info.isFile() || upload_info.size() <= 0)
        return false;
    QFile upload(uploadPath);
    if (!upload.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = upload.readAll();
    upload.close();
    if (data.isEmpty())
        return false;

    const QString md5 = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
    QString suffix = upload_info.suffix().toLower();
    if (suffix == "jpeg")
        suffix = QStringLiteral("jpg");
    if (suffix != "jpg" && suffix != "png")
        suffix = QStringLiteral("png");
    const QString name = md5 + QLatin1Char('.') + suffix; //内容相同即同名，服务器只存一份
    const QString uniqueId = QUuid::createUuid().toString();

    //先把压缩图复制到本机缓存：本地保留的是和服务器同一份高质量图，以后点开大图直接用；
    //缓存失败就不发消息，保留输入区让用户重试，避免本地只剩缩略图
    const QString cache = ImageCachePath(name);
    if (cache.isEmpty()) {
        qWarning() << "[chat] local image cache unavailable";
        return false;
    }
    QDir().mkpath(QFileInfo(cache).absolutePath());
    QFile::remove(cache);
    if (!QFile::copy(uploadPath, cache)) {
        qWarning() << "[chat] cache outgoing image failed:" << cache;
        return false;
    }
    EnsureImageThumbnail(name); //气泡缩略图放在 thumbs 下，不覆盖这份压缩图

    //先让消息出现在会话里；message_id 与 thread_id 等回执补齐
    auto message = std::make_shared<TextChatData>(uniqueId, QString(), mgr->GetUid(),
        conv._peer_uid, 0, conv._thread_id, ChatMessageStatus::Sending);
    message->_msg_type = ChatMsgType::Image;
    message->_resource_name = name;
    message->_resource_md5 = md5;
    message->_local_path = cache;
    message->_imageSource = thumbnail;
    message->_imageWidth = width;
    message->_imageHeight = height;
    message->_transferProgress = 0;
    message->_transfer_state = ResourceTransferState::Uploading;
    mgr->AppendChatMsgs(conv, {message});

    //临时上传文件由 ChatBridge 在整批发送成功后统一删除：同一张图可能被发送两次
    _pending_images.insert(uniqueId, PendingImage{cache, name, md5, conv});

    QJsonObject frame;
    fillConversationFrame(frame, conv);
    frame["md5"] = md5;
    frame["name"] = name;
    frame["token"] = mgr->GetToken();
    frame["unique_id"] = uniqueId;
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_IMG_CHAT_MSG_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
    return true;
}

void ChatService::uploadPendingImage(const QString &uniqueId, qint64 messageId,
                                     const ConversationRef &conv)
{
    auto iter = _pending_images.find(uniqueId);
    if (iter == _pending_images.end())
        return;
    const PendingImage image = iter.value();
    _pending_images.erase(iter);

    FileService::GetInstance()->UploadChatResource(image.name, image.path, image.md5,
        messageId, conv._peer_uid, conv._thread_id, static_cast<int>(conv._kind), uniqueId);
}

void ChatService::discardPendingImage(const QString &uniqueId)
{
    _pending_images.remove(uniqueId); //服务端没落库，图片不需要上传
}

bool ChatService::sendVideo(const ConversationRef &conv, const QString &path,
                            const QString &fileName, qint64 fileSize)
{
    Q_UNUSED(fileSize);
    auto *mgr = UserMgr::GetInstance().get();
    if (!ensureConversation(conv))
        return false;

    const QFileInfo source(path);
    if (!source.isFile() || source.size() <= 0) {
        qWarning() << "[chat] video source unavailable:" << path;
        return false;
    }
    const QString md5 = fileMd5(path);
    if (md5.isEmpty()) {
        qWarning() << "[chat] video md5 failed:" << path;
        return false;
    }
    const QString original = fileName.trimmed().isEmpty() ? source.fileName() : fileName.trimmed();
    //视频按原文件名落盘，重名才追加序号；图片仍按内容 md5 命名
    const QString name = uniqueResourceName(ChatMsgType::Video, path, original, source.size(), md5);
    const QString cache = ResourcePath(ChatMsgType::Video, name);
    if (cache.isEmpty()) {
        qWarning() << "[chat] video cache path unavailable";
        return false;
    }

    if (!QDir().mkpath(QFileInfo(cache).absolutePath())) {
        qWarning() << "[chat] create video dir failed:" << cache;
        return false;
    }
    if (!QFileInfo::exists(cache) || QFileInfo(cache).size() != source.size()) {
        QFile::remove(cache);
        if (!QFile::copy(path, cache)) {
            qWarning() << "[chat] cache video failed:" << path << cache;
            return false;
        }
    }
    const qint64 size = QFileInfo(cache).size();
    const QString uniqueId = QUuid::createUuid().toString();

    auto message = std::make_shared<TextChatData>(uniqueId, QString(), mgr->GetUid(),
        conv._peer_uid, 0, conv._thread_id, ChatMessageStatus::Sending);
    message->_msg_type = ChatMsgType::Video;
    message->_resource_name = name;
    message->_resource_md5 = md5;
    message->_file_name = original;
    message->_file_size = size;
    message->_local_path = cache;
    message->_transfer_state = ResourceTransferState::Preparing;
    mgr->AppendChatMsgs(conv, {message});

    _pending_videos.insert(uniqueId, PendingVideo{conv, cache, name, md5,
        message->_file_name, size});

    //首帧异步提取：拿到后再上传缩略图，缩略图完成后才发 1034
    auto *thumbnailer = new VideoThumbnailer(FileService::GetInstance().get());
    QObject::connect(thumbnailer, &VideoThumbnailer::finished, qApp,
                     [this, thumbnailer, uniqueId](const QImage &image) {
        thumbnailer->deleteLater();
        startVideoMessage(uniqueId, image);
    });
    QObject::connect(thumbnailer, &VideoThumbnailer::failed, qApp,
                     [this, thumbnailer, uniqueId] {
        thumbnailer->deleteLater();
        startVideoMessage(uniqueId, QImage());
    });
    thumbnailer->grab(cache);
    return true;
}

//首帧提取完成后：保存并上传缩略图，等它结束再发消息；失败就直接发无预览的视频
void ChatService::startVideoMessage(const QString &uniqueId, const QImage &image)
{
    auto pending = _pending_videos.constFind(uniqueId);
    if (pending == _pending_videos.constEnd())
        return; //账号已切换或发送已取消

    QString thumbName;
    if (!image.isNull()) {
        const QByteArray bytes = encodeVideoThumb(image);
        if (!bytes.isEmpty()) {
            const QString md5 = QString::fromLatin1(
                QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex());
            thumbName = md5 + QStringLiteral(".jpg");
            const QString thumbPath = VideoThumbPath(thumbName);
            if (!thumbPath.isEmpty()) {
                QDir().mkpath(QFileInfo(thumbPath).absolutePath());
                QFile file(thumbPath);
                if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) {
                    file.close();
                    UserMgr::GetInstance()->SetLocalVideoThumb(pending->conv, uniqueId,
                        thumbPath, image.size());
                    auto message = UserMgr::GetInstance()->FindMessageByUniqueId(uniqueId);
                    if (message)
                        message->_thumb_name = thumbName;

                    _video_by_thumb.insert(thumbName, uniqueId);
                    FileService::GetInstance()->UploadChatResource(thumbName, thumbPath, md5,
                        0, 0, 0, 1, QString());
                    return; //上传完成后由 chatResourceFinished 继续
                }
                file.close();
                QFile::remove(thumbPath);
            }
        }
    }

    sendVideoRequest(uniqueId, thumbName); //没有可用首帧也照常发视频
}

void ChatService::sendVideoRequest(const QString &uniqueId, const QString &thumbName)
{
    auto pending = _pending_videos.find(uniqueId);
    if (pending == _pending_videos.end())
        return;
    const PendingVideo video = pending.value();
    _pending_videos.erase(pending);

    auto message = UserMgr::GetInstance()->FindMessageByUniqueId(uniqueId);
    if (message)
        message->_thumb_name = thumbName;

    PendingResource resource;
    resource.type = ChatMsgType::Video;
    resource.conv = video.conv;
    resource.path = video.path;
    resource.name = video.name;
    resource.md5 = video.md5;
    resource.file_name = video.file_name;
    resource.file_size = video.file_size;
    resource.thumb_name = thumbName;
    resource.unique_id = uniqueId;
    _pending_resources.insert(uniqueId, resource);
    sendResourceRequest(resource);
}

bool ChatService::sendFile(const ConversationRef &conv, const QString &path,
                           const QString &fileName, qint64 fileSize)
{
    Q_UNUSED(fileSize);
    auto *mgr = UserMgr::GetInstance().get();
    if (!ensureConversation(conv))
        return false;

    const QFileInfo source(path);
    if (!source.isFile() || source.size() <= 0) {
        qWarning() << "[chat] file source unavailable:" << path;
        return false;
    }
    const QString md5 = fileMd5(path);
    if (md5.isEmpty()) {
        qWarning() << "[chat] file md5 failed:" << path;
        return false;
    }
    const QString original = fileName.trimmed().isEmpty() ? source.fileName() : fileName.trimmed();
    //文件同样按原文件名保存，下载后可以直接用这个名字打开或另存
    const QString name = uniqueResourceName(ChatMsgType::File, path, original, source.size(), md5);
    const QString cache = ResourcePath(ChatMsgType::File, name);
    if (cache.isEmpty()) {
        qWarning() << "[chat] file cache path unavailable";
        return false;
    }

    if (!QDir().mkpath(QFileInfo(cache).absolutePath())) {
        qWarning() << "[chat] create file dir failed:" << cache;
        return false;
    }
    if (!QFileInfo::exists(cache) || QFileInfo(cache).size() != source.size()) {
        QFile::remove(cache);
        if (!QFile::copy(path, cache)) {
            qWarning() << "[chat] cache file failed:" << path << cache;
            return false;
        }
    }
    const qint64 size = QFileInfo(cache).size();
    const QString uniqueId = QUuid::createUuid().toString();

    auto message = std::make_shared<TextChatData>(uniqueId, QString(), mgr->GetUid(),
        conv._peer_uid, 0, conv._thread_id, ChatMessageStatus::Sending);
    message->_msg_type = ChatMsgType::File;
    message->_resource_name = name;
    message->_resource_md5 = md5;
    message->_file_name = original;
    message->_file_size = size;
    message->_local_path = cache;
    message->_transfer_state = ResourceTransferState::Preparing;
    mgr->AppendChatMsgs(conv, {message});

    PendingResource resource;
    resource.type = ChatMsgType::File;
    resource.conv = conv;
    resource.path = cache;
    resource.name = name;
    resource.md5 = md5;
    resource.file_name = message->_file_name;
    resource.file_size = size;
    resource.unique_id = uniqueId;
    _pending_resources.insert(uniqueId, resource);
    sendResourceRequest(resource);
    return true;
}

void ChatService::sendResourceRequest(const PendingResource &resource)
{
    auto *mgr = UserMgr::GetInstance().get();
    QJsonObject frame;
    fillConversationFrame(frame, resource.conv);
    frame["msg_type"] = static_cast<int>(resource.type);
    frame["name"] = resource.name;
    frame["file_name"] = resource.file_name;
    frame["file_size"] = static_cast<double>(resource.file_size);
    frame["thumb"] = resource.thumb_name;
    frame["md5"] = resource.md5;
    frame["token"] = mgr->GetToken();
    frame["unique_id"] = resource.unique_id;
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_RESOURCE_CHAT_MSG_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

//1035 回执：补齐服务端消息 ID 后把资源本体交给 FileService 上传。
//会话上下文直接取自在途记录，回包里的 touid 在群聊里没有意义
void ChatService::HandleResourceRsp(const QJsonObject &data)
{
    const QString uniqueId = data.value("unique_id").toString();
    auto pending = _pending_resources.find(uniqueId);
    if (pending == _pending_resources.end())
        return;
    const PendingResource resource = pending.value();
    _pending_resources.erase(pending);

    auto *mgr = UserMgr::GetInstance().get();
    if (data.value("error").toInt(1) != 0) {
        mgr->UpdateChatMessage(resource.conv, uniqueId, 0, ChatMessageStatus::Failed);
        mgr->SetResourceStateByUniqueId(resource.conv, uniqueId,
            ResourceTransferState::Failed, -1);
        return;
    }

    ConversationRef conv = resource.conv;
    const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
    if (threadId > 0) {
        conv._thread_id = threadId;
        if (conv._kind == ChatThreadKind::Private && conv._peer_uid > 0)
            mgr->SetThreadId(conv._peer_uid, threadId);
    }
    const qint64 messageId = data.value("message_id").toVariant().toLongLong();
    mgr->UpdateChatMessage(conv, uniqueId, messageId, ChatMessageStatus::Sending);
    mgr->SetResourceStateByUniqueId(conv, uniqueId, ResourceTransferState::Uploading, 0);
    FileService::GetInstance()->UploadChatResource(resource.name, resource.path,
        resource.md5, messageId, conv._peer_uid, conv._thread_id,
        static_cast<int>(conv._kind), uniqueId);
}

//接收方收到 1033：消息先落到会话里，本地有压缩图就直接生成缩略图显示，否则边下载边显示进度。
//群聊的 touid 为 0，改按 thread_type + thread_id 判断归属
void ChatService::HandleImageNotify(const QJsonObject &data)
{
    auto *mgr = UserMgr::GetInstance().get();
    const int sender = data.value("fromuid").toInt();
    const int receiver = data.value("touid").toInt();
    const int threadType = data.value("thread_type").toInt(1);
    const qint64 messageId = data.value("message_id").toVariant().toLongLong();
    const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
    const QString name = data.value("name").toString();
    const QString msgId = data.value("msgid").toString();
    if (sender <= 0 || name.isEmpty() || threadId <= 0)
        return;

    ConversationRef conv;
    conv._thread_id = threadId;
    conv._kind = static_cast<ChatThreadKind>(threadType);
    if (conv._kind == ChatThreadKind::Group) {
        if (!mgr->GetGroup(threadId)) {
            //被邀请后先收到资源通知：先占位并补资料，不能让这张图丢掉
            mgr->EnsureGroup(threadId);
            GroupService::GetInstance()->RequestGroupInfo(threadId);
        }
    }
    else {
        if (receiver != mgr->GetUid() || !mgr->GetFriendById(sender))
            return; //不是好友的历史通知直接丢弃
        conv._peer_uid = sender;
        //对方刚建的会话可能还没同步到本机，这里补上映射
        if (mgr->ThreadId(sender) <= 0)
            mgr->SetThreadId(sender, threadId);
    }

    auto message = std::make_shared<TextChatData>(
        msgId.isEmpty() ? QString::number(messageId) : msgId, QString(),
        sender, conv._peer_uid, messageId, threadId, ChatMessageStatus::Unread);
    message->_msg_type = ChatMsgType::Image;
    message->_resource_name = name;
    message->_transferProgress = 0;
    message->_transfer_state = ResourceTransferState::Downloading;

    QSize pixel;
    const QString thumb = EnsureImageThumbnail(name, &pixel);
    if (!thumb.isEmpty()) {
        message->_imageSource = QUrl::fromLocalFile(thumb).toString();
        message->_imageWidth = pixel.width();
        message->_imageHeight = pixel.height();
        message->_local_path = ImageCachePath(name);
        message->_transferProgress = 100; //本机已有压缩图，不需要再下载
        message->_transfer_state = ResourceTransferState::Done;
    }
    mgr->UpsertResourceMessage(conv, message);

    if (thumb.isEmpty())
        DownloadChatImage(name, sender, messageId, threadId);
}

//接收方收到 1036：只建卡片并下载视频首帧，视频/文件本体等用户点击
void ChatService::HandleResourceNotify(const QJsonObject &data)
{
    auto *mgr = UserMgr::GetInstance().get();
    const int sender = data.value("fromuid").toInt();
    const int receiver = data.value("touid").toInt();
    const int threadType = data.value("thread_type").toInt(1);
    const qint64 messageId = data.value("message_id").toVariant().toLongLong();
    const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
    const QString name = data.value("name").toString();
    const QString msgId = data.value("msgid").toString();
    const int msgType = data.value("msg_type").toInt();
    const QString content = data.value("content").toString();
    if (sender <= 0 || name.isEmpty() || threadId <= 0 || (msgType != 2 && msgType != 3))
        return;

    ConversationRef conv;
    conv._thread_id = threadId;
    conv._kind = static_cast<ChatThreadKind>(threadType);
    if (conv._kind == ChatThreadKind::Group) {
        if (!mgr->GetGroup(threadId)) {
            mgr->EnsureGroup(threadId);
            GroupService::GetInstance()->RequestGroupInfo(threadId);
        }
    }
    else {
        if (receiver != mgr->GetUid() || !mgr->GetFriendById(sender))
            return;
        conv._peer_uid = sender;
        if (mgr->ThreadId(sender) <= 0)
            mgr->SetThreadId(sender, threadId);
    }

    auto message = BuildResourceMessage(sender, conv._peer_uid, messageId, threadId, msgType,
        content, name, ChatMessageStatus::Unread);
    if (!message)
        return;
    if (!msgId.isEmpty())
        message->_msg_id = msgId;
    mgr->UpsertResourceMessage(conv, message);

    if (msgType == 2 && !message->_thumb_name.isEmpty())
        ensureVideoThumb(message->_thumb_name, sender, messageId, threadId);
}

//把一次下载排进队列：同一资源只保留一个任务，实时通知插到队首。
//真正的网络请求由 pumpDownloads 按并发上限放行
void ChatService::enqueueDownload(const ChatDownload &task, bool urgent)
{
    if (task.name.isEmpty())
        return;
    if (_chat_downloads.contains(task.name))
        return;
    for (const auto &queued : _download_queue) {
        if (queued.name == task.name)
            return;
    }
    //同一份资源可能被多条消息引用（同一张图发两次），后到的只补消息进度
    if (urgent)
        _download_queue.prepend(task);
    else
        _download_queue.append(task);
    pumpDownloads();
}

int ChatService::activeDownloadCount() const
{
    int active = 0;
    for (const auto &task : _chat_downloads) {
        if (!task.paused)
            ++active;
    }
    return active;
}

//按并发上限从队列取任务。登录后一次拉很多历史图片，
//同时开满会把资源连接占死，进度条也会一起乱跳
void ChatService::pumpDownloads()
{
    if (!FileService::GetInstance()->IsConnected())
        return;

    while (!_download_queue.isEmpty()
           && activeDownloadCount() < MaxConcurrentImageDownloads) {
        ChatDownload task = _download_queue.takeFirst();
        if (task.path.isEmpty()) {
            task.path = task.thumb ? VideoThumbPath(task.name)
                                   : ResourcePath(task.type, task.name);
        }
        if (task.path.isEmpty())
            continue;

        //本机已经有完整文件：直接发布，不再走网络
        if (task.thumb) {
            if (QFileInfo::exists(task.path)) {
                UserMgr::GetInstance()->SetVideoThumb(task.name, task.path,
                    QImageReader(task.path).size());
                continue;
            }
        }
        else if (task.type == ChatMsgType::Image) {
            QSize pixel;
            const QString thumb = EnsureImageThumbnail(task.name, &pixel);
            if (!thumb.isEmpty()) {
                UserMgr::GetInstance()->FinishResourceDownload(task.name,
                    ImageCachePath(task.name), thumb, pixel);
                continue;
            }
        }
        else if (QFileInfo::exists(task.path)) {
            const qint64 size = QFileInfo(task.path).size();
            if (task.expected_size <= 0 || size == task.expected_size) {
                UserMgr::GetInstance()->FinishResourceDownload(task.name, task.path, {}, {});
                continue;
            }
            QFile::remove(task.path); //上次中断留下的半截文件，重新下载
        }

        if (UserMgr::GetInstance()->GetDownloadFile(task.name))
            continue; //同名头像仍在下，放弃本次任务，重连/重新登录时会再排队

        QDir().mkpath(QFileInfo(task.path).absolutePath());
        auto info = std::make_shared<DownloadFileInfo>();
        info->_name = task.name;
        info->_client_path = task.path;
        info->_owner_uid = task.sender; //资源在发送方的目录下
        info->_seq = 1;
        UserMgr::GetInstance()->AddDownloadFile(info);

        _chat_downloads.insert(task.name, task);
        FileTcpMgr::GetInstance()->StartDownload(task.name);
    }
}

//视频首帧：与图片共用下载队列，但只更新预览，不改变视频本体的下载状态
void ChatService::ensureVideoThumb(const QString &thumbName, int sender, qint64 messageId,
                                   qint64 threadId)
{
    if (thumbName.isEmpty() || sender <= 0)
        return;
    ChatDownload task;
    task.name = thumbName;
    task.sender = sender;
    task.message_id = messageId;
    task.thread_id = threadId;
    task.type = ChatMsgType::Video;
    task.thumb = true;
    enqueueDownload(task, false);
}

//下载完成后的统一发布：图片换成缩略图，视频首帧只换预览，其余记录本地路径
void ChatService::applyImageDownloaded(const ChatDownload &task, const QString &localPath)
{
    if (task.thumb) {
        UserMgr::GetInstance()->SetVideoThumb(task.name, localPath,
            QImageReader(localPath).size());
        return;
    }
    if (task.type == ChatMsgType::Image) {
        QSize pixel;
        const QString thumb = EnsureImageThumbnail(task.name, &pixel);
        if (thumb.isEmpty())
            UserMgr::GetInstance()->FailResourceDownload(task.name);
        else
            UserMgr::GetInstance()->FinishResourceDownload(task.name,
                ImageCachePath(task.name), thumb, pixel);
        return;
    }
    UserMgr::GetInstance()->FinishResourceDownload(task.name, localPath, {}, {});
}

//历史消息或通知里的图片没有本地文件时排入下载队列
void ChatService::DownloadChatImage(const QString &name, int sender, qint64 messageId, qint64 threadId)
{
    if (name.isEmpty() || sender <= 0)
        return;
    ChatDownload task;
    task.name = name;
    task.sender = sender;
    task.message_id = messageId;
    task.thread_id = threadId;
    task.type = ChatMsgType::Image;
    enqueueDownload(task, false);
}

//历史视频只补首帧，视频本体等用户点击
void ChatService::DownloadVideoThumb(const QString &thumbName, int sender, qint64 messageId,
    qint64 threadId)
{
    ensureVideoThumb(thumbName, sender, messageId, threadId);
}

//用户点击视频/文件卡片后手动开始下载
void ChatService::DownloadResource(qint64 messageId)
{
    auto message = UserMgr::GetInstance()->FindMessage(messageId);
    if (!message || message->_resource_name.isEmpty()
        || (message->_msg_type != ChatMsgType::Video && message->_msg_type != ChatMsgType::File))
        return;
    if (message->_transfer_state == ResourceTransferState::Done
        && !LocalPathForMessage(messageId).isEmpty())
        return;
    if (message->_transfer_state == ResourceTransferState::Done) {
        //本地文件被移动或删除：状态退回未下载，点击下载就是重新拉取
        UserMgr::GetInstance()->MarkResourceMissing(messageId);
        message = UserMgr::GetInstance()->FindMessage(messageId);
        if (!message)
            return;
    }
    if (message->_transfer_state == ResourceTransferState::Downloading
        || message->_transfer_state == ResourceTransferState::DownloadPaused)
        return;

    ChatDownload task;
    task.name = message->_resource_name;
    task.sender = message->_from_uid;
    task.message_id = messageId;
    task.thread_id = message->_thread_id;
    task.type = message->_msg_type;
    task.expected_size = message->_file_size;
    enqueueDownload(task, true);
    UserMgr::GetInstance()->SetResourceState(messageId, ResourceTransferState::Downloading, 0);
}

void ChatService::ToggleResourceTransfer(qint64 messageId, const QString &uniqueId)
{
    auto *mgr = UserMgr::GetInstance().get();
    qint64 threadId = 0;
    auto message = mgr->FindMessage(messageId, &threadId);
    if (!message && !uniqueId.isEmpty())
        message = mgr->FindMessageByUniqueId(uniqueId, &threadId);
    if (!message || (message->_msg_type != ChatMsgType::Video
        && message->_msg_type != ChatMsgType::File))
        return;
    //会话上下文按消息号反查：暂停/继续/重试都作用在同一条消息所属的会话上
    ConversationRef conv;
    conv._thread_id = threadId;
    conv._kind = mgr->ThreadKind(threadId);
    conv._peer_uid = conv._kind == ChatThreadKind::Private ? mgr->FriendIdByThread(threadId) : 0;
    messageId = message->_message_id; //1035 之前为 0，后面按 msgid 更新状态
    const bool isSelf = message->_from_uid == mgr->GetUid();
    const QString name = message->_resource_name;
    const auto state = message->_transfer_state;

    if (state == ResourceTransferState::Uploading && isSelf) {
        if (FileService::GetInstance()->PauseResourceUpload(name))
            mgr->SetResourceState(messageId, ResourceTransferState::UploadPaused,
                message->_transferProgress);
        return;
    }
    if (state == ResourceTransferState::UploadPaused && isSelf) {
        if (FileService::GetInstance()->ResumeResourceUpload(name))
            mgr->SetResourceState(messageId, ResourceTransferState::Uploading,
                message->_transferProgress);
        return;
    }
    if (state == ResourceTransferState::Downloading) {
        if (FileService::GetInstance()->PauseDownload(name)) {
            auto it = _chat_downloads.find(name);
            if (it != _chat_downloads.end())
                it->paused = true;
            mgr->SetResourceState(messageId, ResourceTransferState::DownloadPaused,
                message->_transferProgress);
            pumpDownloads(); //暂停不占并发位，让其他资源先走
        }
        return;
    }
    if (state == ResourceTransferState::DownloadPaused) {
        if (FileService::GetInstance()->ResumeDownload(name)) {
            auto it = _chat_downloads.find(name);
            if (it != _chat_downloads.end())
                it->paused = false;
            mgr->SetResourceState(messageId, ResourceTransferState::Downloading,
                message->_transferProgress);
        }
        return;
    }
    if (state == ResourceTransferState::Failed && isSelf && !message->_local_path.isEmpty()) {
        if (message->_message_id > 0) {
            //消息已经落库，只是资源没传完：重新走上传，服务端最后一片写完后仍会通知对方
            mgr->UpdateChatMessage(conv, message->_msg_id, messageId, ChatMessageStatus::Sending);
            mgr->SetResourceState(messageId, ResourceTransferState::Uploading, 0);
            FileService::GetInstance()->UploadChatResource(name, message->_local_path,
                message->_resource_md5, messageId, conv._peer_uid, conv._thread_id,
                static_cast<int>(conv._kind), message->_msg_id);
        }
        else {
            //1035 就没成功，服务端没有这条消息：补发一次 1034，回执里再拿消息 ID
            PendingResource resource;
            resource.type = message->_msg_type;
            resource.conv = conv;
            resource.path = message->_local_path;
            resource.name = name;
            resource.md5 = message->_resource_md5;
            resource.file_name = message->_file_name;
            resource.file_size = message->_file_size;
            resource.thumb_name = message->_thumb_name;
            resource.unique_id = message->_msg_id;
            _pending_resources.insert(resource.unique_id, resource);
            mgr->UpdateChatMessage(conv, message->_msg_id, 0, ChatMessageStatus::Sending);
            mgr->SetResourceStateByUniqueId(conv, message->_msg_id,
                ResourceTransferState::Preparing, 0);
            sendResourceRequest(resource);
        }
        return;
    }
    DownloadResource(messageId); //未下载/失败时点击即开始或重试
}

//登录或重连后：图片补齐完整资源，视频只补首帧；文件与视频本体不自动下载。
//单聊与群聊共用同一套消息结构，遍历时只按 thread 处理，不区分会话类型
void ChatService::RetryImageDownloads()
{
    if (!FileService::GetInstance()->IsConnected())
        return;
    auto *mgr = UserMgr::GetInstance().get();
    auto scan = [](const std::shared_ptr<ChatThreadData> &thread) {
        if (!thread)
            return;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_resource_name.isEmpty())
                continue;
            if (message->_msg_type == ChatMsgType::Image
                && message->_imageSource.isEmpty()) {
                ChatService::GetInstance()->DownloadChatImage(message->_resource_name,
                    message->_from_uid, message->_message_id, thread->_thread_id);
            }
            else if (message->_msg_type == ChatMsgType::Video
                && message->_imageSource.isEmpty() && !message->_thumb_name.isEmpty()) {
                ChatService::GetInstance()->DownloadVideoThumb(message->_thumb_name,
                    message->_from_uid, message->_message_id, thread->_thread_id);
            }
        }
    };
    const int friend_count = mgr->FriendCount();
    for (int i = 0; i < friend_count; ++i) {
        auto friend_info = mgr->FriendAt(i);
        if (friend_info)
            scan(friend_info->_chat_thread);
    }
    const int group_count = mgr->GroupCount();
    for (int i = 0; i < group_count; ++i) {
        auto group = mgr->GroupAt(i);
        if (group)
            scan(group->_chat_thread);
    }
    pumpDownloads(); //队列里可能已经有等待中的任务
}

bool ChatService::ParseResourceMeta(const QString &content, QString *fileName, qint64 *size,
    QString *thumb, QString *resource)
{
    const QJsonDocument document = QJsonDocument::fromJson(content.toUtf8());
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    if (fileName)
        *fileName = object.value("name").toString();
    if (size)
        *size = object.value("size").toVariant().toLongLong();
    if (thumb)
        *thumb = object.value("thumb").toString();
    if (resource)
        *resource = object.value("resource").toString();
    return true;
}

std::shared_ptr<TextChatData> ChatService::BuildResourceMessage(int from, int to,
    qint64 messageId, qint64 threadId, int msgType, const QString &content,
    const QString &resourceName, ChatMessageStatus status)
{
    if (msgType != 2 && msgType != 3)
        return {};
    QString fileName;
    qint64 size = 0;
    QString thumb;
    QString resource = resourceName;
    ParseResourceMeta(content, &fileName, &size, &thumb, &resource);

    auto message = std::make_shared<TextChatData>(QString::number(messageId), QString(),
        from, to, messageId, threadId, status);
    message->_msg_type = msgType == 2 ? ChatMsgType::Video : ChatMsgType::File;
    message->_resource_name = resource;
    message->_file_name = fileName.isEmpty() ? resource : fileName;
    message->_file_size = size;
    message->_thumb_name = thumb;
    message->_transfer_state = ResourceTransferState::None;
    message->_transferProgress = -1;
    return message;
}

QString ChatService::LocalPathForMessage(qint64 messageId)
{
    auto message = UserMgr::GetInstance()->FindMessage(messageId);
    if (!message)
        return {};
    if (!message->_local_path.isEmpty() && QFileInfo::exists(message->_local_path))
        return message->_local_path;
    if (message->_resource_name.isEmpty())
        return {};
    const QString path = ResourcePath(message->_msg_type, message->_resource_name);
    return QFileInfo::exists(path) ? path : QString();
}
