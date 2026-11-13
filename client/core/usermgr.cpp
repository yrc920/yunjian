#include "usermgr.h"

#include "global.h"

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

//账号数据根目录：users/<uid>/
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

    return QDir(root).filePath(QStringLiteral("users/%1").arg(uid));
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

//图片、视频、文件各占一个目录；资源名已经是 <md5>.<ext>，这里只做文件名安全处理
QString UserMgr::ResourcePath(ChatMsgType type, const QString &name)
{
    if (name.isEmpty() || type == ChatMsgType::Text)
        return {};
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    const QString safeName = QFileInfo(name).fileName();
    if (safeName.isEmpty())
        return {};

    QString folder;
    switch (type) {
    case ChatMsgType::Image: folder = QStringLiteral("images"); break;
    case ChatMsgType::Video: folder = QStringLiteral("videos"); break;
    case ChatMsgType::File: folder = QStringLiteral("files"); break;
    default: folder = QStringLiteral("files"); break;
    }
    return QDir(account).filePath(folder + QLatin1Char('/') + safeName);
}

//图片缩略图由本机压缩图生成；视频首帧是发送方上传的独立资源，按原资源名缓存
QString UserMgr::ResourceThumbPath(ChatMsgType type, const QString &name)
{
    if (name.isEmpty())
        return {};
    const QString account = AccountRootPath();
    if (account.isEmpty())
        return {};
    const QString safeName = QFileInfo(name).fileName();
    if (safeName.isEmpty())
        return {};

    if (type == ChatMsgType::Image) {
        const QString base = QFileInfo(name).completeBaseName();
        if (base.isEmpty())
            return {};
        return QDir(account).filePath(QStringLiteral("images/thumbs/%1_thumb.jpg").arg(base));
    }
    if (type == ChatMsgType::Video)
        return QDir(account).filePath(QStringLiteral("videos/thumbs/") + safeName);
    return {};
}

QString UserMgr::ChatImagePath(const QString &name)
{
    return ResourcePath(ChatMsgType::Image, name); //点开大图直接读这张压缩图
}

QString UserMgr::ChatImageThumbPath(const QString &name)
{
    return ResourceThumbPath(ChatMsgType::Image, name);
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

//资源连接断开时聊天资源已经发不出去，把记录整体取走，由 FileService 逐条标记失败
QList<std::shared_ptr<UploadFileInfo>> UserMgr::TakeResourceUploads()
{
    QList<std::shared_ptr<UploadFileInfo>> resources;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto it = _uploads.begin(); it != _uploads.end();) {
        if (it.value() && it.value()->_req_id == ID_UPLOAD_FILE_REQ) {
            resources.append(it.value());
            it = _uploads.erase(it);
        }
        else {
            ++it;
        }
    }
    if (_uploads.isEmpty())
        _upload_paused = false;
    return resources;
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

//资源上传是异步的：网络线程按资源名回推进度，同一资源可能被多条消息引用，
//所以遍历所有会话一次性更新，再按好友逐个通知模型
void UserMgr::UpdateResourceUploadProgress(const QString &resource_name, qint64 trans, qint64 total)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name)
                continue;
            //只推进还在发送中的消息；同内容的旧消息已经完成，不能被这次上传重新改成发送中
            if (message->_status != ChatMessageStatus::Sending)
                continue;
            const qint64 percent = total > 0 ? trans * 100 / total : 0;
            message->_transferProgress = static_cast<int>(qBound<qint64>(0LL, percent, 100LL));
            if (message->_transfer_state == ResourceTransferState::Preparing)
                message->_transfer_state = ResourceTransferState::Uploading;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

void UserMgr::FinishResourceUpload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name
                || message->_status != ChatMessageStatus::Sending)
                continue;
            message->_transferProgress = 100;
            message->_transfer_state = ResourceTransferState::Done;
            message->_status = ChatMessageStatus::Sent;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

void UserMgr::FailResourceUpload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name
                || message->_status != ChatMessageStatus::Sending)
                continue;
            message->_transferProgress = -1;
            message->_transfer_state = ResourceTransferState::Failed;
            message->_status = ChatMessageStatus::Failed;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

std::shared_ptr<UploadFileInfo> UserMgr::GetUploadFileByName(const QString &name) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto it = _uploads.cbegin(); it != _uploads.cend(); ++it) {
        if (it.value() && it.value()->_name == name)
            return it.value();
    }
    return {};
}

//接收方收到资源通知：历史里可能已经有同一 msgid 的消息，只补齐字段，不重复插入
void UserMgr::UpsertResourceMessage(int friend_id, std::shared_ptr<TextChatData> message)
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
        existing->_msg_type = message->_msg_type;
        existing->_resource_name = message->_resource_name;
        existing->_resource_md5 = message->_resource_md5;
        existing->_file_name = message->_file_name;
        existing->_file_size = message->_file_size;
        existing->_thumb_name = message->_thumb_name;
        if (!message->_imageSource.isEmpty()) {
            existing->_imageSource = message->_imageSource;
            existing->_imageWidth = message->_imageWidth;
            existing->_imageHeight = message->_imageHeight;
        }
        if (!message->_local_path.isEmpty())
            existing->_local_path = message->_local_path;
        //已经下载/上传完成的消息不能被一条迟到通知降级回未开始
        if (existing->_transfer_state != ResourceTransferState::Done) {
            existing->_transfer_state = message->_transfer_state;
            existing->_transferProgress = message->_transferProgress;
        }
        info->AppendChatMsgs({existing}); //拿到服务端 ID 后重新排序
        emit FriendChatChanged(friend_id);
        return;
    }
    AppendFriendChatMsg(friend_id, {message});
}

//下载进度只更新还没有完成的资源消息，已经显示出来的图片不再回退
void UserMgr::UpdateResourceDownloadProgress(const QString &resource_name, qint64 current, qint64 total)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name)
                continue;
            if (message->_transfer_state == ResourceTransferState::Done)
                continue;
            const qint64 percent = total > 0 ? current * 100 / total : 0;
            message->_transferProgress = static_cast<int>(qBound<qint64>(0LL, percent, 100LL));
            if (message->_transfer_state != ResourceTransferState::DownloadPaused)
                message->_transfer_state = ResourceTransferState::Downloading;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//下载完成：图片把气泡换成缩略图；视频/文件只记录完整资源位置与完成状态
void UserMgr::FinishResourceDownload(const QString &resource_name, const QString &local_path,
                                     const QString &display_path, const QSize &pixel_size)
{
    if (resource_name.isEmpty())
        return;
    const QString url = display_path.isEmpty()
        ? QString() : QUrl::fromLocalFile(display_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name)
                continue;
            if (!local_path.isEmpty())
                message->_local_path = local_path;
            if (!url.isEmpty()) {
                message->_imageSource = url;
                message->_imageWidth = pixel_size.width();
                message->_imageHeight = pixel_size.height();
            }
            message->_transferProgress = 100;
            message->_transfer_state = ResourceTransferState::Done;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

void UserMgr::FailResourceDownload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name)
                continue;
            if (message->_transfer_state == ResourceTransferState::Done)
                continue;
            message->_transferProgress = -1;
            message->_transfer_state = ResourceTransferState::Failed;
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

//视频首帧是独立资源：下载完成后只换预览，视频本体仍等用户点击下载
void UserMgr::SetVideoThumb(const QString &thumb_name, const QString &local_path,
                            const QSize &pixel_size)
{
    if (thumb_name.isEmpty() || local_path.isEmpty())
        return;
    const QString url = QUrl::fromLocalFile(local_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        bool changed = false;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Video
                || message->_thumb_name != thumb_name)
                continue;
            message->_imageSource = url;
            message->_imageWidth = pixel_size.width();
            message->_imageHeight = pixel_size.height();
            changed = true;
        }
        if (changed)
            emit FriendChatChanged(info->_uid);
    }
}

void UserMgr::SetResourceState(qint64 message_id, ResourceTransferState state, int progress)
{
    if (message_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_message_id != message_id)
                continue;
            message->_transfer_state = state;
            message->_transferProgress = progress;
            emit FriendChatChanged(info->_uid);
            return;
        }
    }
}

void UserMgr::SetResourceStateByUniqueId(int friend_id, const QString &msg_id,
                                         ResourceTransferState state, int progress)
{
    if (msg_id.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto info = GetFriendById(friend_id);
    if (!info)
        return;
    for (const auto &message : info->_chat_thread->_chat_msgs) {
        if (!message || message->_msg_id != msg_id)
            continue;
        message->_transfer_state = state;
        message->_transferProgress = progress;
        emit FriendChatChanged(friend_id);
        return;
    }
}

void UserMgr::SetLocalVideoThumb(int friend_id, const QString &msg_id, const QString &local_path,
                                 const QSize &pixel_size)
{
    if (msg_id.isEmpty() || local_path.isEmpty())
        return;
    const QString url = QUrl::fromLocalFile(local_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto info = GetFriendById(friend_id);
    if (!info)
        return;
    for (const auto &message : info->_chat_thread->_chat_msgs) {
        if (!message || message->_msg_id != msg_id)
            continue;
        message->_imageSource = url;
        message->_imageWidth = pixel_size.width();
        message->_imageHeight = pixel_size.height();
        emit FriendChatChanged(friend_id);
        return;
    }
}

std::shared_ptr<TextChatData> UserMgr::FindMessage(qint64 message_id, int *friend_id) const
{
    if (message_id <= 0)
        return {};
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (message && message->_message_id == message_id) {
                if (friend_id)
                    *friend_id = info->_uid;
                return message;
            }
        }
    }
    return {};
}

std::shared_ptr<TextChatData> UserMgr::FindMessageByUniqueId(const QString &msg_id,
                                                             int *friend_id) const
{
    if (msg_id.isEmpty())
        return {};
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (message && message->_msg_id == msg_id) {
                if (friend_id)
                    *friend_id = info->_uid;
                return message;
            }
        }
    }
    return {};
}

//本地文件被移动/改名后，消息不能继续显示“已下载”；退回未下载让用户重新拉取
void UserMgr::MarkResourceMissing(qint64 message_id)
{
    if (message_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_message_id != message_id)
                continue;
            message->_local_path.clear();
            message->_transfer_state = ResourceTransferState::None;
            message->_transferProgress = -1;
            emit FriendChatChanged(info->_uid);
            return;
        }
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
            //视频/文件可能停在准备或上传阶段，状态一起复位，否则卡片会一直显示“准备中”
            if (message->_msg_type == ChatMsgType::Video || message->_msg_type == ChatMsgType::File) {
                if (message->_transfer_state == ResourceTransferState::Preparing
                    || message->_transfer_state == ResourceTransferState::Uploading
                    || message->_transfer_state == ResourceTransferState::UploadPaused) {
                    message->_transfer_state = ResourceTransferState::Failed;
                    message->_transferProgress = -1;
                }
            }
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
    emit SessionCleared(); //在 uid 被清掉之前通知，业务层还能按旧账号删除半截文件
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
