#include "chatservice.h"

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
#include "filetcpmgr.h"
#include "sessionservice.h"
#include "tcpmgr.h"
#include "usermgr.h"

namespace {
constexpr int ThumbMaxWidth = 480; //与 ComposerBridge 的缩略图尺寸保持一致
constexpr int ThumbMaxHeight = 360;
constexpr int ThumbQuality = 86;
}

ChatService::ChatService()
{
    //资源上传的进度和结果由 FileService 汇总，这里只按资源名更新对应消息；
    //ChatService 不是 QObject，连接的上下文用 FileService，生命周期随它一起结束
    auto *file = FileService::GetInstance().get();
    QObject::connect(file, &FileService::chatImageProgress, file,
            [](const QString &name, qint64 trans, qint64 total) {
        UserMgr::GetInstance()->UpdateImageTransfer(name, trans, total,
            ImageTransferState::Uploading);
    });
    QObject::connect(file, &FileService::chatImageFinished, file, [](const QString &name) {
        UserMgr::GetInstance()->UpdateImageTransfer(name, 1, 1, ImageTransferState::Finished);
    });
    QObject::connect(file, &FileService::chatImageFailed, file, [](const QString &name, int) {
        UserMgr::GetInstance()->UpdateImageTransfer(name, 0, 0, ImageTransferState::Failed);
    });

    //下载结果由资源网络线程发出，用 qApp 做上下文排队回主线程再改消息数据
    auto *file_tcp = FileTcpMgr::GetInstance().get();
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_progress, qApp,
            [this](const QString &name, qint64 current, qint64 total) {
        if (!_chat_downloads.contains(name))
            return; //头像下载不动消息进度
        UserMgr::GetInstance()->UpdateImageDownloadProgress(name, current, total);
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_finished, qApp,
            [this](const QString &name, const QString &) {
        if (!_chat_downloads.contains(name))
            return;
        _chat_downloads.remove(name);
        QSize pixel;
        const QString thumb = EnsureImageThumbnail(name, &pixel);
        if (thumb.isEmpty()) {
            UserMgr::GetInstance()->FailImageDownload(name);
            return;
        }
        UserMgr::GetInstance()->FinishImageDownload(name, thumb, pixel);
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_download_failed, qApp,
            [this](const QString &name, int) {
        if (_chat_downloads.remove(name) > 0)
            UserMgr::GetInstance()->FailImageDownload(name);
    });
    QObject::connect(file_tcp, &FileTcpMgr::sig_connection_closed, qApp, [this] {
        //资源连接断开后这些下载不会再有回包，清掉记录并删掉半截文件
        for (auto it = _chat_downloads.constBegin(); it != _chat_downloads.constEnd(); ++it) {
            UserMgr::GetInstance()->RemoveDownloadFile(it.key());
            QFile::remove(ImageCachePath(it.key()));
            UserMgr::GetInstance()->FailImageDownload(it.key());
        }
        _chat_downloads.clear();
    });
    //登录时图片通知可能先于资源连接到达，连接就绪后统一补发一次；
    //排到事件循环末尾，确保 FileService 已经把 _connected 置为 true
    QObject::connect(file_tcp, &FileTcpMgr::sig_con_success, qApp, [this](bool success) {
        if (success)
            QTimer::singleShot(0, qApp, [this] { RetryImageDownloads(); });
    });
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
bool ChatService::sendText(int uid, const QString &text)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady() || !mgr->GetFriendById(uid))
        return false;
    ensurePrivateChat(uid);
    if (text.isEmpty())
        return true;

    const int from = mgr->GetUid();
    qsizetype offset = 0;
    while (offset < text.size()) {
        const QString msgId = QUuid::createUuid().toString();
        QJsonObject item{{"msgid", msgId}};
        QJsonObject frame{{"fromuid", from}, {"touid", uid}};
        frame["thread_id"] = mgr->ThreadId(uid);
        QByteArray payload;
        QString content;

        // 服务端接收上限为 2048 字节。按 JSON 编码后的字节数分帧，
        // 预留转发时的 error 字段与格式化开销；UTF-16 代理对不可拆开。
        while (offset < text.size()) {
            const qsizetype count = text.at(offset).isHighSurrogate()
                && offset + 1 < text.size() && text.at(offset + 1).isLowSurrogate() ? 2 : 1;
            const QString next = content + text.mid(offset, count);
            item["content"] = next;
            frame["text_array"] = QJsonArray{item};
            const QByteArray encoded = QJsonDocument(frame).toJson(QJsonDocument::Compact);
            if (encoded.size() > 1800)
                break;
            content = next;
            payload = encoded;
            offset += count;
        }

        emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_TEXT_CHAT_MSG_REQ, payload);
        mgr->AppendFriendChatMsg(uid, {
            std::make_shared<TextChatData>(msgId, content, from, uid, 0, mgr->ThreadId(uid), ChatMessageStatus::Sending)
        });
    }
    return true;
}

QString ChatService::ImageCachePath(const QString &name)
{
    return UserMgr::ChatImagePath(name); //账号目录由 UserMgr 统一维护
}

QString ChatService::ImageThumbPath(const QString &name)
{
    return UserMgr::ChatImageThumbPath(name);
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
bool ChatService::sendImage(int uid, const QString &thumbnail, const QString &uploadPath,
    int width, int height)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady() || !mgr->GetFriendById(uid))
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

    ensurePrivateChat(uid);
    //先让消息出现在会话里；message_id 与 thread_id 等回执补齐
    auto message = std::make_shared<TextChatData>(uniqueId, QString(), mgr->GetUid(), uid, 0,
        mgr->ThreadId(uid), ChatMessageStatus::Sending);
    message->_msg_type = ChatMsgType::Image;
    message->_resource_name = name;
    message->_imageSource = thumbnail;
    message->_imageWidth = width;
    message->_imageHeight = height;
    message->_transferProgress = 0;
    mgr->AppendFriendChatMsg(uid, {message});

    //临时上传文件由 ChatBridge 在整批发送成功后统一删除：同一张图可能被发送两次
    _pending_images.insert(uniqueId, PendingImage{cache, name, md5, uid});

    QJsonObject frame{{"fromuid", mgr->GetUid()}, {"touid", uid}};
    frame["thread_id"] = mgr->ThreadId(uid);
    frame["md5"] = md5;
    frame["name"] = name;
    frame["token"] = mgr->GetToken();
    frame["unique_id"] = uniqueId;
    emit TcpMgr::GetInstance()->sig_send_data(ReqId::ID_IMG_CHAT_MSG_REQ,
        QJsonDocument(frame).toJson(QJsonDocument::Compact));
    return true;
}

void ChatService::uploadPendingImage(const QString &uniqueId, qint64 messageId, int touid, qint64 threadId)
{
    auto iter = _pending_images.find(uniqueId);
    if (iter == _pending_images.end())
        return;
    const PendingImage image = iter.value();
    _pending_images.erase(iter);

    FileService::GetInstance()->UploadChatImage(image.name, image.path, image.md5,
        messageId, touid, threadId, uniqueId);
}

void ChatService::discardPendingImage(const QString &uniqueId)
{
    _pending_images.remove(uniqueId); //服务端没落库，图片不需要上传
}

//接收方收到 1033：消息先落到会话里，本地有压缩图就直接生成缩略图显示，否则边下载边显示进度
void ChatService::HandleImageNotify(const QJsonObject &data)
{
    auto *mgr = UserMgr::GetInstance().get();
    const int sender = data.value("fromuid").toInt();
    const int receiver = data.value("touid").toInt();
    const qint64 messageId = data.value("message_id").toVariant().toLongLong();
    const qint64 threadId = data.value("thread_id").toVariant().toLongLong();
    const QString name = data.value("name").toString();
    const QString msgId = data.value("msgid").toString();
    if (sender <= 0 || receiver != mgr->GetUid() || name.isEmpty())
        return;
    if (!mgr->GetFriendById(sender))
        return; //不是好友的历史通知直接丢弃

    //对方刚建的会话可能还没同步到本机，这里补上映射
    if (threadId > 0 && mgr->ThreadId(sender) <= 0)
        mgr->SetThreadId(sender, threadId);

    auto message = std::make_shared<TextChatData>(
        msgId.isEmpty() ? QString::number(messageId) : msgId, QString(),
        sender, receiver, messageId, threadId, ChatMessageStatus::Unread);
    message->_msg_type = ChatMsgType::Image;
    message->_resource_name = name;
    message->_transferProgress = 0;

    QSize pixel;
    const QString thumb = EnsureImageThumbnail(name, &pixel);
    if (!thumb.isEmpty()) {
        message->_imageSource = QUrl::fromLocalFile(thumb).toString();
        message->_imageWidth = pixel.width();
        message->_imageHeight = pixel.height();
        message->_transferProgress = 100; //本机已有压缩图，不需要再下载
    }
    mgr->UpsertImageMessage(sender, message);

    if (thumb.isEmpty())
        DownloadChatImage(name, sender, messageId, threadId);
}

void ChatService::DownloadChatImage(const QString &name, int sender, qint64 messageId, qint64 threadId)
{
    if (name.isEmpty() || sender <= 0)
        return;
    if (UserMgr::GetInstance()->GetDownloadFile(name) || _chat_downloads.contains(name))
        return; //同一资源只保留一次下载，完成后所有引用它的消息一起更新
    if (!FileService::GetInstance()->IsConnected())
        return; //资源连接就绪后由 RetryImageDownloads 补发，避免请求被网络层直接丢弃

    const QString target = ImageCachePath(name);
    if (target.isEmpty())
        return;
    QDir().mkpath(QFileInfo(target).absolutePath());

    auto info = std::make_shared<DownloadFileInfo>();
    info->_name = name;
    info->_client_path = target;
    info->_owner_uid = sender; //资源在发送方的目录下
    info->_seq = 1;
    UserMgr::GetInstance()->AddDownloadFile(info);

    _chat_downloads.insert(name, ChatDownload{sender, messageId, threadId});
    FileTcpMgr::GetInstance()->StartDownload(name);
}

//资源连接就绪后扫描所有还没有本地图片的消息，补发下载；历史消息和新通知共用这一条路径
void ChatService::RetryImageDownloads()
{
    if (!FileService::GetInstance()->IsConnected())
        return;
    auto *mgr = UserMgr::GetInstance().get();
    const int friend_count = mgr->FriendCount();
    for (int i = 0; i < friend_count; ++i) {
        auto friend_info = mgr->FriendAt(i);
        if (!friend_info || !friend_info->_chat_thread)
            continue;
        for (const auto &message : friend_info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Image
                || message->_resource_name.isEmpty() || !message->_imageSource.isEmpty())
                continue;
            DownloadChatImage(message->_resource_name, message->_from_uid,
                message->_message_id, message->_thread_id);
        }
    }
}
