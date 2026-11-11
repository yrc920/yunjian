#include "chatservice.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

#include "fileservice.h"
#include "sessionservice.h"
#include "tcpmgr.h"
#include "usermgr.h"

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
    if (name.isEmpty())
        return {};
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty())
        return {};
    return QDir(root).filePath(QStringLiteral("images/") + QFileInfo(name).fileName());
}

// 图片消息：先插入本地消息并发 1031，服务端落库后回执，再上传资源。
// 返回 true 只代表消息已交给网络层
bool ChatService::sendImage(int uid, const QString &source, int width, int height)
{
    auto *mgr = UserMgr::GetInstance().get();
    if (!SessionService::GetInstance()->isReady() || !mgr->GetFriendById(uid))
        return false;

    //编辑器插入图片时统一转成 PNG data URL，上传用的就是这份数据
    static const QString prefix = QStringLiteral("data:image/png;base64,");
    if (!source.startsWith(prefix))
        return false;
    const QByteArray png = QByteArray::fromBase64(source.mid(prefix.size()).toLatin1());
    if (png.isEmpty())
        return false;

    const QString md5 = QString::fromLatin1(
        QCryptographicHash::hash(png, QCryptographicHash::Md5).toHex());
    const QString name = md5 + QStringLiteral(".png"); //内容相同即同名，服务器只存一份
    const QString uniqueId = QUuid::createUuid().toString();

    ensurePrivateChat(uid);
    //先让消息出现在会话里；message_id 与 thread_id 等回执补齐
    auto message = std::make_shared<TextChatData>(uniqueId, QString(), mgr->GetUid(), uid, 0,
        mgr->ThreadId(uid), ChatMessageStatus::Sending);
    message->_msg_type = ChatMsgType::Image;
    message->_resource_name = name;
    message->_imageSource = source;
    message->_imageWidth = width;
    message->_imageHeight = height;
    mgr->AppendFriendChatMsg(uid, {message});

    //回执前把图片数据留在内存，按 unique_id 取回后落盘上传
    _pending_images.insert(uniqueId, PendingImage{png, name, md5, uid});

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

    //落盘到本机缓存：既是上传源，也让重新登录后的历史消息能直接显示
    const QString path = ImageCachePath(image.name);
    if (path.isEmpty())
        return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[chat] save outgoing image failed:" << path;
        return;
    }
    const bool written = file.write(image.data) == image.data.size();
    file.close();
    if (!written) {
        qWarning() << "[chat] write outgoing image failed:" << path;
        return;
    }

    FileService::GetInstance()->UploadChatImage(image.name, path, image.md5,
        messageId, touid, threadId, uniqueId);
}

void ChatService::discardPendingImage(const QString &uniqueId)
{
    _pending_images.remove(uniqueId); //服务端没落库，图片不需要上传
}
