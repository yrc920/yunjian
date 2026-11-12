#include "usermgr.h"

#include "global.h"

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

namespace {
constexpr qint64 MaxAvatarBytes = 4 * 1024 * 1024;

//头像文件按 PNG 签名和 IEND 尾块校验：下载中断留下的半截文件不能当图片用
bool isAvatarPng(const QByteArray &png)
{
    static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    if (png.size() <= signature.size() + 8 || png.size() > MaxAvatarBytes
        || !png.startsWith(signature))
        return false;
    return png.mid(png.size() - 8, 4) == QByteArrayLiteral("IEND");
}

//直接给 data URL：同一路径覆盖保存后 Image 会命中旧缓存，data URL 每次都是新内容
QString avatarSource(const QByteArray &png)
{
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}
}

UserMgr::UserMgr() : _user_info(nullptr)
{

}

UserMgr::~UserMgr() {}

//登录后先恢复本机头像；服务端 icon 仍保留，换设备时作为兜底
void UserMgr::SetUserInfo(std::shared_ptr<UserInfo> user_info)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    const QString previousIcon = GetIcon();
    _user_info = std::move(user_info);
    _localIcon.clear();
    CleanPendingImages(); //新会话没有草稿，上次退出遗留的待上传图可以安全清掉
    const QString path = LocalAvatarPath();
    if (!path.isEmpty()) {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.size() <= MaxAvatarBytes) {
            const QByteArray png = file.read(MaxAvatarBytes + 1);
            if (isAvatarPng(png))
                _localIcon = avatarSource(png);
        }
    }
    if (GetIcon() != previousIcon)
        emit UserIconChanged();
}

std::shared_ptr<UserInfo> UserMgr::GetUserInfo()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _user_info;
}

int UserMgr::GetUid()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _user_info ? _user_info->_uid : 0;
}

void UserMgr::SetToken(const QString &token)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _token = token; //资源服务器复用聊天登录下发的token
}

QString UserMgr::GetToken() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _token;
}

//账号数据根目录：users/<网关摘要>/<uid>/
//头像、好友头像缓存、聊天图片和临时上传图都放在这里，本地登录多个账号也不会互相串
QString UserMgr::AccountRootPath()
{
    auto mgr = GetInstance();
    std::lock_guard<std::recursive_mutex> lock(mgr->_mtx);
    const int uid = mgr->_user_info ? mgr->_user_info->_uid : 0;
    if (uid <= 0)
        return {};

    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty())
        return {};

    const QUrl gateway(gate_url_prefix);
    const QString endpoint = gateway.host().toLower() + QLatin1Char(':')
        + QString::number(gateway.port(gateway.scheme() == QStringLiteral("https") ? 443 : 80));
    const QString scope = QString::fromLatin1(
        QCryptographicHash::hash(endpoint.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(root).filePath(QStringLiteral("users/%1/%2").arg(scope).arg(uid));
}

QString UserMgr::LocalAvatarPath()
{
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    return QDir(account).filePath(QStringLiteral("avatar.png"));
}

//好友头像同样按登录账号隔离，避免换账号后读到上一个账号下载的缓存
QString UserMgr::ResourceCachePath(int ownerUid, const QString &name)
{
    if (ownerUid <= 0 || name.isEmpty())
        return {};

    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};

    const QString safeName = QFileInfo(name).fileName();
    if (safeName.isEmpty())
        return {};
    return QDir(account).filePath(QStringLiteral("contacts/%1/%2").arg(ownerUid).arg(safeName));
}

//聊天图片保留的是和服务器同一份压缩图，点开大图直接读这里
QString UserMgr::ChatImagePath(const QString &name)
{
    if (name.isEmpty())
        return {};
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    return QDir(account).filePath(QStringLiteral("images/") + QFileInfo(name).fileName());
}

QString UserMgr::ChatImageThumbPath(const QString &name)
{
    if (name.isEmpty())
        return {};
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    const QString base = QFileInfo(name).completeBaseName();
    if (base.isEmpty())
        return {};
    return QDir(account).filePath(QStringLiteral("images/thumbs/%1_thumb.jpg").arg(base));
}

QString UserMgr::PendingImageDir()
{
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    return QDir(account).filePath(QStringLiteral("images/pending"));
}

//登录时本账号还没有任何草稿，遗留的 pending 文件都是上次退出/崩溃留下的，可以安全清掉
void UserMgr::CleanPendingImages()
{
    const QString dir = PendingImageDir();
    if (dir.isEmpty())
        return;
    QDir pending(dir);
    if (!pending.exists())
        return;
    const auto files = pending.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const auto &info : files)
        QFile::remove(info.absoluteFilePath());
}

//下载中断、进程被杀都会留下半截文件；只校验非空会把残图当成已缓存，之后不再重试
bool UserMgr::IsCompleteAvatarFile(const QString &path)
{
    if (path.isEmpty())
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    if (file.size() <= 16 || file.size() > MaxAvatarBytes)
        return false;

    static const QByteArray signature("\x89PNG\r\n\x1a\n", 8);
    if (file.read(8) != signature)
        return false;

    //IEND是PNG最后一个块，文件被截断时读不到；只认最后8字节里的块类型
    if (!file.seek(file.size() - 8))
        return false;
    return file.read(4) == QByteArrayLiteral("IEND");
}

QString UserMgr::GetIcon() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (!_user_info)
        return {}; //还没登录
    return _localIcon.isEmpty() ? _user_info->_icon : _localIcon; //本机优先，缺失回落服务端
}

//只更新内存中的显示头像；写盘由裁剪器完成，失败时不发布变更
bool UserMgr::SetLocalAvatar(const QByteArray &png)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (GetUid() <= 0 || !isAvatarPng(png))
        return false;

    const QString source = avatarSource(png);
    if (_localIcon != source) {
        _localIcon = source;
        emit UserIconChanged();
    }
    return true;
}

void UserMgr::AddUploadFile(std::shared_ptr<UploadFileInfo> info)
{
    if (!info || info->_md5.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _uploads[info->_md5] = std::move(info); //回包按md5查，重复上传覆盖旧状态
}

std::shared_ptr<UploadFileInfo> UserMgr::GetUploadFile(const QString &md5)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _uploads.value(md5);
}

void UserMgr::RemoveUploadFile(const QString &md5)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _uploads.remove(md5);
    if (_uploads.isEmpty())
        _upload_paused = false; //没有在途任务，暂停标志复位
}

//资源连接断开时聊天图片已经发不出去，把记录整体取走，由 FileService 逐条标记失败
QList<std::shared_ptr<UploadFileInfo>> UserMgr::TakeImageUploads()
{
    QList<std::shared_ptr<UploadFileInfo>> images;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto it = _uploads.begin(); it != _uploads.end();) {
        if (it.value() && it.value()->_req_id == ID_UPLOAD_FILE_REQ) {
            images.append(it.value());
            it = _uploads.erase(it);
        }
        else {
            ++it;
        }
    }
    if (_uploads.isEmpty())
        _upload_paused = false;
    return images;
}

void UserMgr::SetUploadPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _upload_paused = paused; //只改标志，已在途的分片仍会回包
}

bool UserMgr::UploadPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _upload_paused;
}

void UserMgr::AddDownloadFile(std::shared_ptr<DownloadFileInfo> info)
{
    if (!info || info->_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _downloads[info->_name] = std::move(info); //同名资源只下载一次
}

std::shared_ptr<DownloadFileInfo> UserMgr::GetDownloadFile(const QString &name)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _downloads.value(name);
}

void UserMgr::RemoveDownloadFile(const QString &name)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    _downloads.remove(name); //完成或失败都必须移除，否则同名资源不会再重试
}

void UserMgr::SetContactIcon(int uid, const QString &icon)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    bool friend_changed = false; //好友列表是否有变化
    bool apply_changed = false; //申请列表是否有变化

    auto iter = _friend_map.find(uid);
    if (iter != _friend_map.end() && iter.value()->_icon != icon) {
        iter.value()->_icon = icon; //好友行换成缓存路径
        friend_changed = true;
    }

    for (auto &apply : _apply_list) {
        //同一 uid 可能有收到/发出两条申请，全部一起刷新
        if (apply->_uid == uid && apply->_icon != icon) {
            apply->_icon = icon; //申请行也换，否则只有好友列表变
            apply_changed = true;
        }
    }

    if (friend_changed)
        emit FriendIconChanged(uid); //只刷新好友列表对应行
    if (apply_changed)
        emit ApplyIconChanged(uid); //只刷新申请列表对应行
}

void UserMgr::AppendApplyList(QJsonArray array)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit ApplyListAboutToChange();
    for (const QJsonValue &value : array) {
        auto name = value["name"].toString();
        auto desc = value["desc"].toString();
        auto icon = value["icon"].toString();
        auto nick = value["nick"].toString();
        auto sex = value["sex"].toInt();
        auto uid = value["uid"].toInt();
        auto status = value["status"].toInt();
        auto direction = value["direction"].toInt();
        auto info = std::make_shared<ApplyInfo>(uid, name, desc, icon, nick, sex, status, direction);
        _apply_list.push_back(info);
    }

    emit ApplyListChanged();
}

void UserMgr::AppendFriendList(QJsonArray array) {
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit FriendListAboutToChange();
    for (const QJsonValue& value : array) {
        auto name = value["name"].toString();
        auto desc = value["desc"].toString();
        auto icon = value["icon"].toString();
        auto nick = value["nick"].toString();
        auto sex = value["sex"].toInt();
        auto uid = value["uid"].toInt();
        auto back = value["back"].toString();

        auto info = std::make_shared<FriendInfo>(uid, name, nick, icon, sex, desc, back);
        _friend_list.push_back(info);
        _friend_map.insert(uid, info);
    }

    emit FriendListChanged();
}

bool UserMgr::AlreadyApply(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for(auto& apply: _apply_list){
        //只统计别人申请我的方向；我发出去的申请不能拦住对方的新申请
        if(apply->_uid == uid && apply->_direction == 0){
            return true;
        }
    }
    return false;
}

bool UserMgr::AlreadySentApply(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto &apply : _apply_list) {
        if (apply->_uid == uid && apply->_direction == 1 && apply->_status == 0)
            return true;
    }
    return false;
}

void UserMgr::AddApplyList(std::shared_ptr<ApplyInfo> app)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit ApplyListAboutToChange();
    _apply_list.push_back(std::move(app));
    emit ApplyListChanged();
}

// 我发出的申请：同一个人只保留一条，重复申请时更新留言与状态
void UserMgr::AddSentApply(int uid, const QString &name, const QString &nick,
                           const QString &icon, const QString &desc)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto &apply : _apply_list) {
        if (apply->_uid != uid || apply->_direction != 1)
            continue;
        emit ApplyListAboutToChange();
        apply->_name = name;
        apply->_nick = nick;
        apply->_icon = icon;
        apply->_desc = desc;
        apply->_status = 0;
        emit ApplyListChanged();
        return;
    }

    emit ApplyListAboutToChange();
    _apply_list.push_back(std::make_shared<ApplyInfo>(uid, name, desc, icon, nick, 0, 0, 1));
    emit ApplyListChanged();
}

void UserMgr::SetApplyStatus(int uid, int status, int direction)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto &apply : _apply_list) {
        if (apply->_uid != uid || apply->_direction != direction || apply->_status == status)
            continue;
        emit ApplyListAboutToChange();
        apply->_status = status;
        emit ApplyListChanged();
        return;
    }
}

bool UserMgr::CheckFriendById(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _friend_map.contains(uid);
}

void UserMgr::AddFriend(std::shared_ptr<AuthRsp> auth_rsp)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto friend_info = std::make_shared<FriendInfo>(auth_rsp);
    if (_friend_map.contains(friend_info->_uid))
        return;
    emit FriendListAboutToChange();
    _friend_map[friend_info->_uid] = friend_info;
    _friend_list.push_back(friend_info); //列表与索引必须同时维护，否则新好友不会出现在列表里
    emit FriendListChanged();
}

void UserMgr::AddFriend(std::shared_ptr<AuthInfo> auth_info)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto friend_info = std::make_shared<FriendInfo>(auth_info);
    if (_friend_map.contains(friend_info->_uid))
        return;
    emit FriendListAboutToChange();
    _friend_map[friend_info->_uid] = friend_info;
    _friend_list.push_back(friend_info);
    emit FriendListChanged();
}

void UserMgr::AppendFriendChatMsg(int friend_id, std::vector<std::shared_ptr<TextChatData>> msgs)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_iter = _friend_map.find(friend_id);
    if(find_iter == _friend_map.end()){
        qDebug()<<"append friend uid  " << friend_id << " not found";
        return;
    }

    find_iter.value()->AppendChatMsgs(msgs);
    emit FriendChatChanged(friend_id);
}

//登录返回的会话列表是当前账号的完整结果，先清空旧映射再逐条写入
void UserMgr::SetChatThreads(const QJsonArray &threads)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit FriendListAboutToChange();
    for (auto &friend_info : _friend_list)
        friend_info->_chat_thread->_thread_id = 0;
    AppendChatThreads(threads);
    emit FriendListChanged();
}

//只补 thread_id：会话里的消息由 1029 单独拉取
void UserMgr::AppendChatThreads(const QJsonArray &threads)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &value : threads) {
        const QJsonObject item = value.toObject();
        const int uid = item.value("uid").toInt();
        auto info = GetFriendById(uid);
        if (!info)
            continue;
        info->_chat_thread->_thread_id = item.value("thread_id").toVariant().toLongLong();
    }
}

void UserMgr::RefreshFriendList()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit FriendListAboutToChange();
    emit FriendListChanged();
}

//无论创建成功还是失败都要清掉 pending，否则该好友之后不会再重试建会话
void UserMgr::SetThreadId(int friend_id, qint64 thread_id)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto info = GetFriendById(friend_id);
    _pending_private_chats.remove(friend_id);
    if (thread_id <= 0 || !info || info->_chat_thread->_thread_id == thread_id)
        return;
    emit FriendListAboutToChange();
    info->_chat_thread->_thread_id = thread_id;
    emit FriendListChanged();
}

bool UserMgr::HasPendingPrivateChat(int friend_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _pending_private_chats.value(friend_id, false);
}

void UserMgr::MarkPrivateChatPending(int friend_id, bool pending)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (pending)
        _pending_private_chats[friend_id] = true;
    else
        _pending_private_chats.remove(friend_id);
}

//发送回执按 msgid 找到原消息，补上服务端 ID 和状态；
//待确认消息原本排在末尾，拿到 ID 后要重新排序才能落到正确位置
void UserMgr::UpdateChatMessage(int friend_id, const QString &msg_id, qint64 message_id,
                                ChatMessageStatus status)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto info = GetFriendById(friend_id);
    if (!info)
        return;
    for (auto &message : info->_chat_thread->_chat_msgs) {
        if (message->_msg_id != msg_id)
            continue;
        if (message_id > 0)
            message->_message_id = message_id;
        message->_status = status;
        info->AppendChatMsgs({message});
        emit FriendChatChanged(friend_id);
        return;
    }
}

//图片上传是异步的：网络线程按资源名回推进度，同一张图可能被多条消息引用，
//所以遍历所有会话一次性更新，再按好友逐个通知模型
void UserMgr::UpdateImageTransfer(const QString &resource_name, qint64 trans, qint64 total,
                                  ImageTransferState state)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Image
                || message->_resource_name != resource_name)
                continue;
            //只推进还在发送中的消息；同内容的旧消息已经完成，不能被这次上传重新改成发送中
            if (message->_status != ChatMessageStatus::Sending)
                continue;
            if (state == ImageTransferState::Uploading) {
                const qint64 percent = total > 0 ? trans * 100 / total : 0;
                message->_transferProgress = static_cast<int>(qBound<qint64>(0LL, percent, 100LL));
            }
            else {
                message->_transferProgress = state == ImageTransferState::Finished ? 100 : -1;
                message->_status = state == ImageTransferState::Finished
                    ? ChatMessageStatus::Sent : ChatMessageStatus::Failed;
            }
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//接收方收到图片通知：历史里可能已经有同一 msgid 的消息，只补齐图片字段，不重复插入
void UserMgr::UpsertImageMessage(int friend_id, std::shared_ptr<TextChatData> message)
{
    if (!message)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto info = GetFriendById(friend_id);
    if (!info)
        return;

    for (auto &existing : info->_chat_thread->_chat_msgs) {
        if (!existing || existing->_msg_id != message->_msg_id)
            continue;
        if (message->_message_id > 0)
            existing->_message_id = message->_message_id;
        if (message->_thread_id > 0)
            existing->_thread_id = message->_thread_id;
        existing->_msg_type = ChatMsgType::Image;
        existing->_resource_name = message->_resource_name;
        if (!message->_imageSource.isEmpty()) {
            existing->_imageSource = message->_imageSource;
            existing->_imageWidth = message->_imageWidth;
            existing->_imageHeight = message->_imageHeight;
        }
        if (message->_transferProgress >= 0)
            existing->_transferProgress = message->_transferProgress;
        info->AppendChatMsgs({existing}); //拿到服务端 ID 后重新排序
        emit FriendChatChanged(friend_id);
        return;
    }
    AppendFriendChatMsg(friend_id, {message});
}

//下载进度只更新还没有完成的图片消息，已经显示出来的图片不再回退
void UserMgr::UpdateImageDownloadProgress(const QString &resource_name, qint64 current, qint64 total)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Image
                || message->_resource_name != resource_name)
                continue;
            if (message->_transferProgress >= 100)
                continue;
            const qint64 percent = total > 0 ? current * 100 / total : 0;
            message->_transferProgress = static_cast<int>(qBound<qint64>(0LL, percent, 100LL));
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//下载完成：压缩图已经在账号目录里，生成缩略图并把所有引用这张图的消息换成缩略图地址
void UserMgr::FinishImageDownload(const QString &resource_name, const QString &thumb_path,
                                  const QSize &pixel_size)
{
    if (resource_name.isEmpty() || thumb_path.isEmpty())
        return;
    const QString url = QUrl::fromLocalFile(thumb_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Image
                || message->_resource_name != resource_name)
                continue;
            message->_imageSource = url;
            message->_imageWidth = pixel_size.width();
            message->_imageHeight = pixel_size.height();
            message->_transferProgress = 100;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

void UserMgr::FailImageDownload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Image
                || message->_resource_name != resource_name)
                continue;
            if (message->_transferProgress >= 100)
                continue;
            message->_transferProgress = -1; //保留占位，等下次进入会话再重试
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//断线时把所有还在「发送中」的消息标失败，避免界面一直转圈
void UserMgr::FailPendingMessages()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (message->_status != ChatMessageStatus::Sending)
                continue;
            message->_status = ChatMessageStatus::Failed;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//会话与好友是 1:1 映射，两个方向互相查找；都只在已加好友范围内有效
qint64 UserMgr::ThreadId(int friend_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto it = _friend_map.constFind(friend_id);
    return it == _friend_map.cend() ? 0 : it.value()->_chat_thread->_thread_id;
}

int UserMgr::FriendIdByThread(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list)
        if (info->_chat_thread->_thread_id == thread_id)
            return info->_uid;
    return 0;
}

//历史消息按最大 message_id 继续翻页；待确认消息 ID 为 0，不参与游标
qint64 UserMgr::LastMessageId(int friend_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto it = _friend_map.constFind(friend_id);
    if (it == _friend_map.cend() || it.value()->_chat_thread->_chat_msgs.empty())
        return 0;
    qint64 lastId = 0;
    for (const auto &message : it.value()->_chat_thread->_chat_msgs)
        lastId = qMax(lastId, message->_message_id);
    return lastId;
}

int UserMgr::FriendCount() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return static_cast<int>(_friend_list.size());
}

std::shared_ptr<FriendInfo> UserMgr::FriendAt(int index) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (index < 0 || index >= static_cast<int>(_friend_list.size()))
        return nullptr;
    return _friend_list[index];
}

std::shared_ptr<FriendInfo> UserMgr::GetFriendById(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_it = _friend_map.find(uid);
    if(find_it == _friend_map.end()){
        return nullptr;
    }

    return *find_it;
}

int UserMgr::ApplyCount() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return static_cast<int>(_apply_list.size());
}

std::shared_ptr<ApplyInfo> UserMgr::ApplyAt(int index) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (index < 0 || index >= static_cast<int>(_apply_list.size()))
        return nullptr;
    return _apply_list[index];
}

//换账号或返回登录时清空全部会话数据；模型通过成对的 AboutToChange/Changed 重建
void UserMgr::ClearSession()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    const bool hadIcon = !GetIcon().isEmpty();
    emit ApplyListAboutToChange();
    emit FriendListAboutToChange();
    _user_info.reset();
    _token.clear();
    _localIcon.clear();
    _apply_list.clear();
    _friend_list.clear();
    _friend_map.clear();
    _pending_private_chats.clear();
    _uploads.clear();
    _downloads.clear();
    _upload_paused = false;

    emit ApplyListChanged();
    emit FriendListChanged();
    if (hadIcon)
        emit UserIconChanged();
}
