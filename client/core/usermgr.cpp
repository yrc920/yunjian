#include "log.h"
#include "usermgr.h"

#include "global.h"
#include "localstore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextDocument>
#include <QUrl>
#include <algorithm>

namespace {
constexpr qint64 MaxAvatarBytes = 4 * 1024 * 1024;

//草稿是带格式的 HTML，会话列表只显示一行纯文本。空段落与换行都压成空格，
//并截断到 120 字，避免一条很长的草稿把整行布局撑坏。
//空文档的 toHtml() 也会返回完整 HTML 骨架，不能只看"html 非空"就当成草稿
QString draftPreview(const QString &html)
{
    if (html.isEmpty())
        return {};
    QTextDocument doc;
    doc.setHtml(html);
    const QString raw = doc.toPlainText();
    //图片/表情在文本里是对象替换符；只有它们没有文字时，仍算一条草稿
    const bool has_object = raw.contains(QChar::ObjectReplacementCharacter);
    QString text = raw;
    text.remove(QChar::ObjectReplacementCharacter);
    text.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text = text.simplified();
    if (text.isEmpty())
        return has_object ? QStringLiteral("草稿") : QString();
    return text.left(120);
}

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

//内存消息转成库行：只存事实。本地路径、缩略图地址、传输进度都能从资源名推导，
//存进库反而会在换机器或移动目录后变成脏数据
StoredMessage storedFromMessage(const std::shared_ptr<TextChatData> &message, qint64 thread_id)
{
    StoredMessage stored;
    if (!message)
        return stored;
    stored.server_id = message->_message_id;
    //合成键只用于内存去重，不写进库：库里空的客户端消息号就是空的
    stored.client_msg_id = message->_msg_id.startsWith(QLatin1String("srv:"))
        ? QString() : message->_msg_id;
    stored.thread_id = thread_id;
    stored.sender_id = message->_from_uid;
    stored.recv_id = message->_to_uid;
    stored.msg_type = static_cast<int>(message->_msg_type);
    stored.content = message->_msg_content;
    stored.mentions = message->_mentions.join(QLatin1Char(','));
    stored.created_at = message->_time;
    stored.status = static_cast<int>(message->_status);
    stored.is_read = message->_status == ChatMessageStatus::Read;
    stored.recalled = message->_recalled;
    stored.recalled_text = message->_recalled_text;
    stored.resource_name = message->_resource_name;
    stored.resource_md5 = message->_resource_md5;
    stored.file_name = message->_file_name;
    stored.file_size = message->_file_size;
    stored.thumb_name = message->_thumb_name;
    return stored;
}

StoredFriend storedFromFriend(const FriendInfo &info, qint64 added_at)
{
    StoredFriend stored;
    stored.uid = info._uid;
    stored.name = info._name;
    stored.nick = info._nick;
    stored.remark = info._back;
    stored.icon = info._icon;
    stored.sex = info._sex;
    stored.signature = info._desc;
    stored.is_friend = info._is_friend;
    stored.added_at = added_at;
    stored.tags = info._tags;
    return stored;
}

StoredGroup storedFromGroup(const GroupInfo &group)
{
    StoredGroup stored;
    stored.thread_id = group._thread_id;
    stored.name = group._name;
    stored.notice = group._notice;
    stored.owner_id = group._owner_id;
    stored.my_role = group._my_role;
    stored.my_nick = group._my_nick;
    stored.muted = group._muted;
    stored.left_state = group._left_state;
    stored.member_count = group._member_count;
    stored.created_at = group._created_at;
    return stored;
}

StoredMember storedFromMember(qint64 thread_id, const GroupMember &member)
{
    StoredMember stored;
    stored.thread_id = thread_id;
    stored.uid = member._uid;
    stored.role = member._role;
    stored.group_nick = member._group_nick;
    stored.joined_at = member._joined_at;
    stored.name = member._name;
    stored.nick = member._nick;
    stored.icon = member._icon;
    stored.sex = member._sex;
    return stored;
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

//本人资料改成功: 更新本地缓存并通知界面; 昵称与签名允许清空, 性别 0 表示不公开
void UserMgr::UpdateMyProfile(const QString &nick, int sex, const QString &desc)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (!_user_info)
        return;
    _user_info->_nick = nick;
    _user_info->_sex = sex;
    _user_info->_desc = desc;
    emit UserProfileChanged();
}

//好友备注与标签改成功: 只改自己这一侧的显示名与分组, 对方的昵称与别人给他的备注都不变
bool UserMgr::UpdateFriendRelation(int uid, const QString &remark, const QStringList &tags)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_it = _friend_map.find(uid);
    if (find_it == _friend_map.end() || !(*find_it))
        return false;
    (*find_it)->_back = remark;
    (*find_it)->_tags = tags;
    LocalStore::GetInstance()->SaveFriend(storedFromFriend(**find_it, 0));
    LocalStore::GetInstance()->SaveFriendTags(uid, tags);
    emit FriendRelationChanged(uid);
    emit FriendTagsChanged(); //标签集合可能新增或消失，筛选条要重算
    return true;
}

//好友文字资料变了：昵称、签名、性别按推送值覆盖。
//头像不在这里更新：_icon 可能是已经下载好的本地路径，改由 FileService 下载完成后统一替换，
//避免推送到达时头像先变成文件名、等下载完才恢复
bool UserMgr::UpdateFriendProfile(int uid, const QString &nick, const QString &desc, int sex)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_it = _friend_map.find(uid);
    if (find_it == _friend_map.end() || !(*find_it))
        return false;

    bool changed = false;
    if ((*find_it)->_nick != nick) {
        (*find_it)->_nick = nick;
        changed = true;
    }
    if ((*find_it)->_desc != desc) {
        (*find_it)->_desc = desc;
        changed = true;
    }
    if (sex >= 0 && (*find_it)->_sex != sex) {
        (*find_it)->_sex = sex;
        changed = true;
    }

    if (changed) {
        LocalStore::GetInstance()->SaveFriend(storedFromFriend(**find_it, 0));
        emit FriendProfileChanged(uid);
    }
    return changed;
}

//对端已读：只处理自己发的、已经有服务端消息号的那些，回执没回来的不参与。
//群聊不做逐条已读，这条路径只服务单聊
bool UserMgr::MarkMessagesReadTo(int peer_uid, qint64 last_read_id)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (last_read_id <= 0)
        return false;
    const int self_uid = _user_info ? _user_info->_uid : 0;

    auto find_it = _friend_map.find(peer_uid);
    const qint64 thread_id = find_it != _friend_map.end() && (*find_it)
        ? (*find_it)->_chat_thread->_thread_id : 0;
    //库里先改：窗口只保留最近一页，早于窗口的自己的消息不在内存里，也要变成已读
    if (thread_id > 0)
        LocalStore::GetInstance()->MarkReadTo(thread_id, self_uid, last_read_id);

    if (find_it == _friend_map.end() || !(*find_it))
        return false;

    bool changed = false;
    for (const auto &msg : (*find_it)->_chat_thread->_chat_msgs) {
        if (!msg || msg->_from_uid != self_uid)
            continue;
        if (msg->_message_id <= 0 || msg->_message_id > last_read_id)
            continue;
        if (msg->_status == ChatMessageStatus::Read)
            continue;
        msg->_status = ChatMessageStatus::Read;
        changed = true;
    }

    if (changed)
        emit ConversationChanged((*find_it)->_chat_thread->_thread_id, peer_uid);
    return changed;
}

//撤回：正文与资源一起清掉，只留一条可渲染的提示，避免旧图片还留在界面或磁盘上被点开。
//消息号全局唯一，先找单聊再找群聊，调用方不需要知道会话类型
bool UserMgr::MarkMessageRecalled(qint64 message_id)
{
    if (message_id <= 0)
        return false;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    const int self_uid = _user_info ? _user_info->_uid : 0;
    QString recalled_text;
    auto recall = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return false;
        for (const auto &msg : thread->_chat_msgs) {
            if (!msg || msg->_message_id != message_id || msg->_recalled)
                continue;
            msg->_recalled = true;
            //自己撤回时留一份原文，供「重新编辑」；别人撤回时本地本来就没有原文
            if (msg->_from_uid == self_uid) {
                msg->_recalled_text = msg->_msg_content;
                recalled_text = msg->_recalled_text;
            }
            msg->_msg_content.clear();
            msg->_msg_type = ChatMsgType::Text;
            msg->_imageSource.clear();
            msg->_imageWidth = 0;
            msg->_imageHeight = 0;
            msg->_file_name.clear();
            msg->_file_size = 0;
            msg->_thumb_name.clear();
            msg->_transferProgress = -1;
            msg->_transfer_state = ResourceTransferState::None;
            emit ConversationChanged(thread->_thread_id, peer);
            return true;
        }
        return false;
    };

    bool found = false;
    //遍历 _friend_map 而不是 _friend_list：已删好友但保留历史的会话也在这个索引里
    for (auto it = _friend_map.cbegin(); it != _friend_map.cend() && !found; ++it) {
        if (it.value() && recall(it.value()->_chat_thread, it.key()))
            found = true;
    }
    for (const auto &group : _group_list) {
        if (found)
            break;
        if (group && recall(group->_chat_thread, 0))
            found = true;
    }

    //内存窗口只保留最近一页，更早的消息不在里面，但库里必须一起改：
    //否则切回会话重新读库时，撤回的正文又会出现
    LocalStore::GetInstance()->MarkRecalled(message_id, recalled_text);
    return found;
}

//发送超时或回执报错：只改状态，内容保留，用户可以直接重发
bool UserMgr::MarkMessageFailed(const ConversationRef &conv, const QString &msg_id)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return false;

    for (const auto &msg : thread->_chat_msgs) {
        if (!msg || msg->_msg_id != msg_id || msg->_status != ChatMessageStatus::Sending)
            continue;
        msg->_status = ChatMessageStatus::Failed;
        emit ConversationChanged(thread->_thread_id, conv._peer_uid);
        return true;
    }
    return false;
}

//重发前先移除旧的失败记录：沿用同一条会让界面出现两条相同内容
bool UserMgr::RemoveChatMessage(const ConversationRef &conv, const QString &msg_id)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return false;

    auto &msgs = thread->_chat_msgs;
    auto iter = std::find_if(msgs.begin(), msgs.end(), [&msg_id](const auto &msg) {
        return msg && msg->_msg_id == msg_id;
    });
    if (iter == msgs.end())
        return false;
    msgs.erase(iter);
    if (thread->_thread_id > 0)
        LocalStore::GetInstance()->DeleteMessage(thread->_thread_id, msg_id);
    emit ConversationChanged(thread->_thread_id, conv._peer_uid);
    return true;
}

//删好友失败时把人放回联系人列表：对象一直在 _friend_map 里，恢复只是重新挂回列表
void UserMgr::RestoreFriend(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_it = _friend_map.find(uid);
    if (find_it == _friend_map.end() || !(*find_it) || (*find_it)->_is_friend)
        return;

    const bool listed = std::any_of(_friend_list.begin(), _friend_list.end(),
        [uid](const auto &info) { return info && info->_uid == uid; });
    if (listed)
        return;

    emit FriendListAboutToChange();
    (*find_it)->_is_friend = true;
    _friend_list.push_back(*find_it);
    LocalStore::GetInstance()->SetFriendActive(uid, true);
    emit FriendListChanged();
    emit FriendTagsChanged();
}

//未读只对"别人发来的消息"累加；正在看的会话由 SetActiveThread 直接清零，不计数
void UserMgr::IncThreadUnread(qint64 thread_id)
{
    if (thread_id <= 0 || thread_id == _active_thread)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    //被本地删除的会话收到新消息时自动回到列表，不能因为隐藏而漏掉消息
    if (auto data = findThreadData(thread_id); data && data->_hidden) {
        data->_hidden = false;
        LocalStore::GetInstance()->SaveThreadFlags(thread_id, data->_pinned,
                                                   data->_muted, false);
        emit ConversationChanged(thread_id, FriendIdByThread(thread_id));
    }
    ++_unread[thread_id];
    LocalStore::GetInstance()->SaveThreadUnread(thread_id, _unread.value(thread_id, 0),
                                                _at_me_threads.contains(thread_id));
    emit UnreadChanged();
}

void UserMgr::ClearThreadUnread(qint64 thread_id)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    bool changed = _unread.remove(thread_id) > 0;
    //@提醒跟着未读一起清：会话行不该在读完消息后还挂着"[有人@我]"
    changed = _at_me_threads.remove(thread_id) || changed;
    LocalStore::GetInstance()->SaveThreadUnread(thread_id, 0, false);
    if (changed)
        emit UnreadChanged();
}

int UserMgr::ThreadUnread(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _unread.value(thread_id, 0);
}

bool UserMgr::ThreadAtMe(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _at_me_threads.contains(thread_id);
}

void UserMgr::MarkThreadAtMe(qint64 thread_id)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (_at_me_threads.contains(thread_id))
        return;
    _at_me_threads.insert(thread_id);
    LocalStore::GetInstance()->SaveThreadUnread(thread_id, _unread.value(thread_id, 0), true);
    emit UnreadChanged();
}

void UserMgr::EnsureGroup(qint64 thread_id)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (_group_map.contains(thread_id))
        return;
    auto group = std::make_shared<GroupInfo>(thread_id);
    group->_name = QStringLiteral("群聊");
    _group_map[thread_id] = group;
    _group_list.push_back(group);
    LocalStore::GetInstance()->SaveGroup(storedFromGroup(*group));
    emit GroupListChanged();
}

int UserMgr::UnreadTotal() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    int total = 0;
    for (auto count : _unread)
        total += count;
    return total;
}

//导航角标用的总数：免打扰的群与单聊都不计入，会话行仍然自己显示小圆点。
//这样"消息免打扰"才真的不打扰，而不是只把会话行的数字换成圆点
int UserMgr::UnreadBadgeTotal() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    int total = 0;
    for (auto it = _unread.cbegin(); it != _unread.cend(); ++it) {
        auto group = _group_map.constFind(it.key());
        if (group != _group_map.constEnd() && *group && (*group)->_muted)
            continue;
        auto data = findThreadData(it.key());
        if (data && data->_muted)
            continue;
        total += it.value();
    }
    return total;
}

//切换当前会话：进入时清零未读，离开时置0；同一会话重复调用不会重复通知
void UserMgr::SetActiveThread(qint64 thread_id)
{
    //从资料页或通知重新打开一个被隐藏的会话时，先让它回到列表
    if (thread_id > 0) {
        std::lock_guard<std::recursive_mutex> lock(_mtx);
        if (auto data = findThreadData(thread_id); data && data->_hidden) {
            data->_hidden = false;
            LocalStore::GetInstance()->SaveThreadFlags(thread_id, data->_pinned,
                                                       data->_muted, false);
            emit ConversationChanged(thread_id, FriendIdByThread(thread_id));
        }
    }
    if (_active_thread == thread_id)
        return;
    _active_thread = thread_id;
    if (thread_id > 0)
        ClearThreadUnread(thread_id);
}

//置顶只改本机顺序：会话模型收到 ConversationChanged 后重新排序
void UserMgr::SetThreadPinned(qint64 thread_id, bool pinned)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data || data->_pinned == pinned)
        return;
    data->_pinned = pinned;
    LocalStore::GetInstance()->SaveThreadFlags(thread_id, pinned, data->_muted, data->_hidden);
    emit ConversationChanged(thread_id, FriendIdByThread(thread_id));
}

//单聊免打扰：群聊的免打扰由群资料页写服务端，不走这里
void UserMgr::SetThreadMuted(qint64 thread_id, bool muted)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data || data->_muted == muted)
        return;
    data->_muted = muted;
    LocalStore::GetInstance()->SaveThreadFlags(thread_id, data->_pinned, muted, data->_hidden);
    emit ThreadStateChanged(thread_id);
    emit UnreadChanged(); //导航角标要跳过免打扰的会话
}

//删除会话只在本机隐藏并清空消息，好友关系与服务端记录都不动
void UserMgr::SetThreadHidden(qint64 thread_id, bool hidden)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data || data->_hidden == hidden)
        return;
    data->_hidden = hidden;
    LocalStore::GetInstance()->SaveThreadFlags(thread_id, data->_pinned, data->_muted, hidden);
    if (hidden) {
        _unread.remove(thread_id);
        _at_me_threads.remove(thread_id);
        LocalStore::GetInstance()->SaveThreadUnread(thread_id, 0, false);
        if (_active_thread == thread_id)
            _active_thread = 0;
        //会话被删掉后不再需要为它保留消息窗口
        if (_window_thread == thread_id)
            _window_thread = 0;
        emit UnreadChanged();
    }
    emit ConversationChanged(thread_id, FriendIdByThread(thread_id));
}

//手动标为未读：至少 1 条；隐藏的会话同时回到列表
void UserMgr::MarkThreadUnread(qint64 thread_id, int count)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (data && data->_hidden) {
        data->_hidden = false;
        LocalStore::GetInstance()->SaveThreadFlags(thread_id, data->_pinned,
                                                   data->_muted, false);
        emit ConversationChanged(thread_id, FriendIdByThread(thread_id));
    }
    _unread[thread_id] = qMax(1, count);
    LocalStore::GetInstance()->SaveThreadUnread(thread_id, _unread.value(thread_id, 0),
                                                _at_me_threads.contains(thread_id));
    emit UnreadChanged();
}

//所有好友标签去重后按名称排序，空标签不会出现在筛选条里
QStringList UserMgr::AllTags() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    QStringList all;
    for (const auto& info : _friend_list) {
        if (!info)
            continue;
        for (const auto& tag : info->_tags) {
            if (!tag.isEmpty() && !all.contains(tag))
                all.append(tag);
        }
    }
    all.sort();
    return all;
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
        auto id = value["id"].toVariant().toLongLong();
        auto apply = std::make_shared<ApplyInfo>(uid, name, desc, icon, nick,
            sex, status, direction, id);
        //重连会重新下发完整申请列表：同一个人同一方向只保留一条，
        //否则断线一次，待处理申请就会翻一倍
        auto existing = std::find_if(_apply_list.begin(), _apply_list.end(),
            [uid, direction](const auto &item) {
                return item && item->_uid == uid && item->_direction == direction;
            });
        if (existing != _apply_list.end())
            *existing = std::move(apply);
        else
            _apply_list.push_back(std::move(apply));
    }
    //新的申请排在上面：登录首屏也按 id 倒序，和收到通知时的插入方向一致
    std::stable_sort(_apply_list.begin(), _apply_list.end(),
        [](const auto &left, const auto &right) { return left->_id > right->_id; });
    _apply_seq = 0;
    for (const auto &apply : _apply_list)
        if (apply)
            _apply_seq = qMax(_apply_seq, apply->_id);

    emit ApplyListChanged();
}

void UserMgr::AppendFriendList(QJsonArray array) {
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit FriendListAboutToChange();
    QSet<int> incoming;
    QVector<StoredFriend> stored;
    qint64 order = 0;
    for (const QJsonValue& value : array) {
        auto name = value["name"].toString();
        auto desc = value["desc"].toString();
        auto icon = value["icon"].toString();
        auto nick = value["nick"].toString();
        auto sex = value["sex"].toInt();
        auto uid = value["uid"].toInt();
        auto back = value["back"].toString();
        if (uid <= 0)
            continue;
        //标签随好友列表下发；缺字段时是空数组，不影响旧数据
        QStringList tags;
        for (const auto& tag : value["tags"].toArray()) {
            const QString text = tag.toString();
            if (!text.isEmpty() && !tags.contains(text))
                tags.append(text);
        }

        incoming.insert(uid);
        //本地缓存里已经有这个人时原地更新：登录前恢复过一份，直接 push 会出现两行
        auto info = _friend_map.value(uid);
        if (!info) {
            info = std::make_shared<FriendInfo>(uid, name, nick, icon, sex, desc, back);
            _friend_list.push_back(info);
            _friend_map.insert(uid, info);
        } else {
            info->_name = name;
            info->_nick = nick;
            info->_icon = icon;
            info->_sex = sex;
            info->_desc = desc;
            info->_back = back;
            info->_is_friend = true;
        }
        info->_tags = tags;
        stored.push_back(storedFromFriend(*info, ++order));
    }

    //服务端的好友表是完整快照：本地有、这次没下发的说明关系已经不在（对方删了我，
    //或我在别处删了）。只把联系人项摘掉，对象与历史会话继续保留。
    //回包为空时不做任何推断：那更可能是服务端异常，不是"好友全没了"
    if (!array.isEmpty()) {
        for (const auto &info : _friend_list) {
            if (!info || !info->_is_friend || incoming.contains(info->_uid))
                continue;
            info->_is_friend = false;
            stored.push_back(storedFromFriend(*info, ++order));
        }
        _friend_list.erase(std::remove_if(_friend_list.begin(), _friend_list.end(),
            [](const auto &info) { return info && !info->_is_friend; }),
            _friend_list.end());
    }

    LocalStore::GetInstance()->SaveFriends(stored);
    emit FriendListChanged();
    emit FriendTagsChanged(); //整表重建后筛选条按新的标签并集刷新
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

//收到新申请：同一个人只保留一条，更新后挪到最前面（列表按新旧从上往下）
void UserMgr::AddApplyList(std::shared_ptr<ApplyInfo> app)
{
    if (!app)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    emit ApplyListAboutToChange();
    app->_id = ++_apply_seq;
    auto existing = std::find_if(_apply_list.begin(), _apply_list.end(),
        [&app](const auto &item) {
            return item && item->_uid == app->_uid && item->_direction == app->_direction;
        });
    if (existing != _apply_list.end())
        _apply_list.erase(existing);
    _apply_list.insert(_apply_list.begin(), std::move(app));
    emit ApplyListChanged();
}

// 我发出的申请：同一个人只保留一条，重复申请时更新留言与状态
void UserMgr::AddSentApply(int uid, const QString &name, const QString &nick,
                           const QString &icon, const QString &desc)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto it = _apply_list.begin(); it != _apply_list.end(); ++it) {
        if (!(*it) || (*it)->_uid != uid || (*it)->_direction != 1)
            continue;
        emit ApplyListAboutToChange();
        auto apply = *it;
        apply->_name = name;
        apply->_nick = nick;
        apply->_icon = icon;
        apply->_desc = desc;
        apply->_status = 0;
        apply->_id = ++_apply_seq;
        _apply_list.erase(it);
        _apply_list.insert(_apply_list.begin(), apply); //刚发出的申请排最上面
        emit ApplyListChanged();
        return;
    }

    emit ApplyListAboutToChange();
    _apply_list.insert(_apply_list.begin(), std::make_shared<ApplyInfo>(
        uid, name, desc, icon, nick, 0, 0, 1, ++_apply_seq));
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

//对方目前是否仍是好友：删好友后对象仍留在 _friend_map 里承载会话，
//所以必须看 _is_friend，否则搜索时会把已删的人当成好友，导致无法重新申请
bool UserMgr::CheckFriendById(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto it = _friend_map.constFind(uid);
    return it != _friend_map.constEnd() && *it && (*it)->_is_friend;
}

//把已有对象恢复成好友：删过好友后对象还留在 _friend_map 里（会话与历史要保留），
//重新通过验证时不能直接 return，否则联系人列表永远少这个人
static void restoreFriendEntry(std::vector<std::shared_ptr<FriendInfo>> &list,
                               const std::shared_ptr<FriendInfo> &info)
{
    if (!info)
        return;
    info->_is_friend = true;
    const bool listed = std::any_of(list.begin(), list.end(),
        [&info](const auto &item) { return item && item->_uid == info->_uid; });
    if (!listed)
        list.push_back(info);
}

void UserMgr::AddFriend(std::shared_ptr<AuthRsp> auth_rsp)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto existing = _friend_map.value(auth_rsp->_uid);
    if (existing) {
        emit FriendListAboutToChange();
        existing->_name = auth_rsp->_name;
        existing->_nick = auth_rsp->_nick;
        existing->_icon = auth_rsp->_icon;
        existing->_sex = auth_rsp->_sex;
        existing->_back = auth_rsp->_back;
        if (!auth_rsp->_tags.isEmpty())
            existing->_tags = auth_rsp->_tags;
        restoreFriendEntry(_friend_list, existing);
        LocalStore::GetInstance()->SaveFriend(storedFromFriend(*existing, 0));
        if (!auth_rsp->_tags.isEmpty())
            LocalStore::GetInstance()->SaveFriendTags(existing->_uid, existing->_tags);
        emit FriendListChanged();
        emit FriendTagsChanged();
        return;
    }
    auto friend_info = std::make_shared<FriendInfo>(auth_rsp);
    emit FriendListAboutToChange();
    _friend_map[friend_info->_uid] = friend_info;
    _friend_list.push_back(friend_info); //列表与索引必须同时维护，否则新好友不会出现在列表里
    LocalStore::GetInstance()->SaveFriend(storedFromFriend(
        *friend_info, QDateTime::currentSecsSinceEpoch()));
    emit FriendListChanged();
}

void UserMgr::AddFriend(std::shared_ptr<AuthInfo> auth_info)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto existing = _friend_map.value(auth_info->_uid);
    if (existing) {
        emit FriendListAboutToChange();
        existing->_name = auth_info->_name;
        existing->_nick = auth_info->_nick;
        existing->_icon = auth_info->_icon;
        existing->_sex = auth_info->_sex;
        existing->_back = auth_info->_back;
        if (!auth_info->_tags.isEmpty())
            existing->_tags = auth_info->_tags;
        restoreFriendEntry(_friend_list, existing);
        LocalStore::GetInstance()->SaveFriend(storedFromFriend(*existing, 0));
        if (!auth_info->_tags.isEmpty())
            LocalStore::GetInstance()->SaveFriendTags(existing->_uid, existing->_tags);
        emit FriendListChanged();
        emit FriendTagsChanged();
        return;
    }
    auto friend_info = std::make_shared<FriendInfo>(auth_info);
    emit FriendListAboutToChange();
    _friend_map[friend_info->_uid] = friend_info;
    _friend_list.push_back(friend_info);
    LocalStore::GetInstance()->SaveFriend(storedFromFriend(
        *friend_info, QDateTime::currentSecsSinceEpoch()));
    emit FriendListChanged();
}

//按会话追加消息：单聊写好友对象里的容器，群聊写群对象。
//找到容器后统一走 ChatThreadData::AppendMsgs，去重、覆盖回执字段与排序只维护一份
void UserMgr::AppendChatMsgs(const ConversationRef &conv,
                             std::vector<std::shared_ptr<TextChatData>> msgs)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread) {
        qCDebug(lcApp) << "append chat msg, thread" << conv._thread_id << "not found";
        return;
    }
    if (conv._thread_id > 0)
        thread->_thread_id = conv._thread_id;

    const qint64 thread_id = thread->_thread_id;
    auto store = LocalStore::GetInstance();

    //先把整页落库：内存只是当前窗口，库才是这个会话的全量历史。
    //游标与消息同事务提交，崩溃后不会出现"游标前进了、消息缺一页"
    if (thread_id > 0) {
        QVector<StoredMessage> stored;
        stored.reserve(static_cast<int>(msgs.size()));
        qint64 cursor = 0;
        for (const auto &message : msgs) {
            if (!message)
                continue;
            stored.push_back(storedFromMessage(message, thread_id));
            cursor = qMax(cursor, message->_message_id);
        }
        store->SaveMessages(thread_id, static_cast<int>(kind), conv._peer_uid, stored, cursor);
    }

    //只有窗口里正在显示的会话保留整段消息；其余会话只留最新一条做列表预览。
    //这里必须用 _window_thread 而不是 _active_thread：后者在切窗口/上翻时会变成 0，
    //拿它判断会把正在看的历史替换成一条
    const bool keep_window = thread_id > 0 && thread_id == _window_thread;
    if (keep_window) {
        //回填的历史页整体比窗口更早，只落库、不进内存：硬插进头部会让正在看的人跳位。
        //窗口为空时（首次打开、库里还没有消息）照常追加，让首屏能显示出来
        const qint64 window_max = thread->_chat_msgs.empty() || !thread->_chat_msgs.back()
            ? 0 : thread->_chat_msgs.back()->_message_id;
        std::vector<std::shared_ptr<TextChatData>> newer;
        newer.reserve(msgs.size());
        for (const auto &message : msgs) {
            if (!message)
                continue;
            if (window_max <= 0 || message->_message_id <= 0
                || message->_message_id > window_max)
                newer.push_back(message);
        }
        if (!newer.empty())
            thread->AppendMsgs(newer);
    } else {
        std::shared_ptr<TextChatData> newest;
        for (const auto &message : msgs) {
            if (!message)
                continue;
            if (!newest || message->_time > newest->_time
                || (message->_time == newest->_time
                    && message->_message_id > newest->_message_id))
                newest = message;
        }
        if (!newest)
            return;
        const auto &head = thread->_chat_msgs.empty() ? nullptr : thread->_chat_msgs.back();
        const bool replace = !head || newest->_time > head->_time
            || (newest->_time == head->_time && newest->_message_id > head->_message_id);
        if (replace) {
            thread->_chat_msgs.clear();
            thread->_chat_msgs.push_back(newest);
        }
    }

    //预览字段跟最后一条一起更新：重启后不必为每个会话再查一次消息
    if (thread_id > 0 && !thread->_chat_msgs.empty()) {
        const auto &last = thread->_chat_msgs.back();
        if (last) {
            store->SaveThreadPreview(thread_id, last->_time, last->_msg_content,
                                     static_cast<int>(last->_msg_type),
                                     last->_from_uid, last->_file_name,
                                     last->_recalled);
        }
    }

    emit ConversationChanged(thread_id, conv._peer_uid);
}

//本机删除：库已删完后同步内存窗口与列表预览，避免会话行继续显示已删正文
void UserMgr::RemoveChatMsgs(qint64 thread_id, const QSet<qint64> &server_ids,
                             const QSet<QString> &client_ids)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data)
        return;

    auto &msgs = data->_chat_msgs;
    const size_t before = msgs.size();
    msgs.erase(std::remove_if(msgs.begin(), msgs.end(),
                              [&](const std::shared_ptr<TextChatData> &msg) {
                                  if (!msg)
                                      return true;
                                  return (msg->_message_id > 0
                                          && server_ids.contains(msg->_message_id))
                                      || (!msg->_msg_id.isEmpty()
                                          && client_ids.contains(msg->_msg_id));
                              }),
               msgs.end());
    if (msgs.size() == before)
        return;

    const int peer = FriendIdByThread(thread_id);
    if (msgs.empty()) {
        if (peer > 0) {
            if (auto friend_info = _friend_map.value(peer)) {
                friend_info->_last_msg.clear();
                friend_info->_last_time = 0;
            }
        }
        if (auto group = _group_map.value(thread_id))
            group->_last_time = 0;
    }
    emit ConversationChanged(thread_id, peer);
}

//登录返回的会话列表是分页的，这里只负责把本页的会话合并进来。
//不能像旧实现那样先把所有好友的 thread_id 清零：第二页之后的会话会短暂失去映射，
//本地缓存恢复出来的映射也会被一起抹掉，表现为"重启后有些会话点不开"
void UserMgr::SetChatThreads(const QJsonArray &threads)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    AppendChatThreads(threads);
    emit FriendListChanged();
}

//会话列表同时含单聊与群聊：按 thread_type 分流，未读与最后活动时间都按 thread 记
void UserMgr::AppendChatThreads(const QJsonArray &threads)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &value : threads) {
        const QJsonObject item = value.toObject();
        const qint64 thread_id = item.value("thread_id").toVariant().toLongLong();
        if (thread_id <= 0)
            continue;
        if (item.value("thread_type").toInt(1) == static_cast<int>(ChatThreadKind::Group)) {
            UpsertGroupThread(item);
            continue;
        }

        const int uid = item.value("uid").toInt();
        if (uid <= 0)
            continue;
        auto info = GetFriendById(uid);
        if (!info) {
            //已删好友但服务端仍有会话：保留一个"非好友"对象，会话列表照样能看到历史
            info = std::make_shared<FriendInfo>(uid, item.value("name").toString(),
                item.value("nick").toString(), item.value("icon").toString(),
                item.value("sex").toInt(), QString(), QString());
            info->_is_friend = false;
            _friend_map[uid] = info;
            //落一条最小资料：否则重启后这个会话没有对象可挂，历史也读不回来
            LocalStore::GetInstance()->SaveFriend(storedFromFriend(*info, 0));
        }
        info->_chat_thread->_thread_id = thread_id;
        //会话最后活动时间只用于首屏排序，拉回历史后会被真实消息时间覆盖
        info->_last_time = item.value("time").toVariant().toLongLong();
        //未读以服务端为准：登录时恢复，之后由本地收到消息与打开会话增减
        const int unread = item.value("unread").toInt(0);
        if (unread > 0)
            _unread[thread_id] = unread;
        else
            _unread.remove(thread_id);

        auto store = LocalStore::GetInstance();
        store->EnsureThread(thread_id, static_cast<int>(ChatThreadKind::Private), uid);
        store->SaveThreadTime(thread_id, info->_last_time);
        store->SaveThreadUnread(thread_id, unread, _at_me_threads.contains(thread_id));
    }
    emit UnreadChanged();
}

//把本地库装回内存：好友、群、成员、会话映射与每条会话的预览。
//这一步在 2002 之前执行，网络回来之前列表就已经有内容，不必等服务端逐个下发
void UserMgr::RestoreFromCache()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto store = LocalStore::GetInstance();
    if (!store->IsOpen())
        return;

    const auto friends = store->LoadFriends();
    for (const auto &item : friends) {
        auto info = std::make_shared<FriendInfo>(item.uid, item.name, item.nick, item.icon,
                                                 item.sex, item.signature, item.remark);
        info->_tags = item.tags;
        info->_is_friend = item.is_friend;
        info->_chat_thread->_thread_id = 0;
        _friend_map.insert(item.uid, info);
        if (item.is_friend)
            _friend_list.push_back(info);
    }

    const auto groups = store->LoadGroups();
    for (const auto &item : groups) {
        auto group = std::make_shared<GroupInfo>(item.thread_id);
        group->_name = item.name;
        group->_notice = item.notice;
        group->_owner_id = item.owner_id;
        group->_my_role = item.my_role;
        group->_my_nick = item.my_nick;
        group->_muted = item.muted;
        group->_left_state = item.left_state;
        group->_member_count = item.member_count;
        group->_created_at = item.created_at;
        _group_map.insert(item.thread_id, group);
        _group_list.push_back(group);
    }

    //成员整表读回：气泡上的发送者名字、@ 候选与群昵称都靠它，不必等 5004
    for (const auto &item : store->LoadMembers()) {
        auto group = _group_map.value(item.thread_id);
        if (!group)
            continue;
        group->_member_map.insert(item.uid,
            GroupMember(item.uid, item.name, item.nick, item.icon, item.sex,
                        item.role, item.group_nick, item.joined_at));
    }

    const auto threads = store->LoadThreads();
    for (const auto &item : threads) {
        std::shared_ptr<ChatThreadData> data;
        if (item.thread_type == static_cast<int>(ChatThreadKind::Group)) {
            auto group = _group_map.value(item.thread_id);
            if (!group)
                continue;
            group->_last_time = item.last_time;
            group->_unread = item.unread;
            group->_at_me = item.at_me;
            data = group->_chat_thread;
        } else {
            auto info = _friend_map.value(item.peer_uid);
            if (!info)
                continue; //没有这个人的资料，会话行渲染不出来
            info->_chat_thread->_thread_id = item.thread_id;
            info->_last_time = item.last_time;
            info->_last_msg = item.last_text; //模型在窗口为空时用它兜底
            data = info->_chat_thread;
        }
        if (!data)
            continue;
        data->_thread_id = item.thread_id;
        data->_pinned = item.pinned;
        //群免打扰来自 grp，不覆盖；thread.muted 只服务单聊
        data->_muted = item.thread_type == static_cast<int>(ChatThreadKind::Group)
            ? false : item.muted;
        data->_hidden = item.hidden;
        data->_draft_preview = draftPreview(item.draft);
        if (item.unread > 0)
            _unread[item.thread_id] = item.unread;
        if (item.at_me)
            _at_me_threads.insert(item.thread_id);

        //预览只放形状，不读资源、不触发下载；真正打开会话时再按窗口取消息
        if (!item.last_text.isEmpty() || item.last_type != 0) {
            auto message = std::make_shared<TextChatData>(QString(), item.last_text,
                item.last_sender, item.peer_uid, 0, item.thread_id,
                ChatMessageStatus::Sent);
            message->_msg_type = static_cast<ChatMsgType>(item.last_type);
            message->_time = item.last_time;
            message->_file_name = item.last_name;
            message->_recalled = item.last_recalled;
            data->_chat_msgs.push_back(message);
        }
    }

    emit FriendListChanged();
    emit GroupListChanged();
    emit UnreadChanged();
    emit FriendTagsChanged();
}

//打开会话：用库里最近一页替换内存窗口。库里才是全量，内存只保留看得见的一段，
//会话切走再切回时重新取，长期使用内存不会随历史线性增长
void UserMgr::SetThreadWindow(qint64 thread_id,
                              std::vector<std::shared_ptr<TextChatData>> msgs)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data)
        return;
    data->_thread_id = thread_id;
    data->_chat_msgs = std::move(msgs);
    //从这里开始，这条会话的消息窗口就由内存保留；切走/失焦/上翻都不改变它
    _window_thread = thread_id;
}

//上滑翻页：先腾空再按"新页 + 旧窗口"重新合并，复用同一套去重与排序规则。
//只发 MessagesPrepended，不发 ConversationChanged —— 后者会被模型当成整表重排
void UserMgr::PrependChatMsgs(qint64 thread_id,
                              const std::vector<std::shared_ptr<TextChatData>> &msgs)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data || msgs.empty())
        return;

    auto existing = data->_chat_msgs;
    data->_chat_msgs.clear();
    data->AppendMsgs(msgs);
    data->AppendMsgs(existing);

    const int added = static_cast<int>(data->_chat_msgs.size())
        - static_cast<int>(existing.size());
    if (added > 0)
        emit MessagesPrepended(thread_id, added);
}


QString UserMgr::ThreadDraft(qint64 thread_id) const
{
    return LocalStore::GetInstance()->LoadDraft(thread_id);
}

void UserMgr::SetThreadDraft(qint64 thread_id, const QString &html)
{
    if (thread_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    LocalStore::GetInstance()->SaveThreadDraft(thread_id, html);
    if (auto data = findThreadData(thread_id)) {
        data->_draft_preview = draftPreview(html);
        emit ThreadStateChanged(thread_id);
    }
}

//被移出群或群解散：对象和历史都保留，只记一个状态。
//以前是直接连消息一起删，用户再想翻记录就没有了
void UserMgr::MarkGroupLeft(qint64 thread_id, int state)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group || state <= 0 || group->_left_state == state)
        return;
    group->_left_state = state;
    _unread.remove(thread_id);
    _at_me_threads.remove(thread_id);
    auto store = LocalStore::GetInstance();
    store->SaveGroup(storedFromGroup(*group));
    store->SaveThreadUnread(thread_id, 0, false);
    emit GroupInfoChanged(thread_id);
    emit GroupListChanged();
    emit UnreadChanged();
}

//登录时做差集：本地有、服务端会话列表里没有的群，说明已被移出或解散。
//只标状态不删数据——用户还能翻历史，什么时候清由他自己决定
void UserMgr::MarkMissingGroupsLeft(const QSet<qint64> &alive_threads)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto store = LocalStore::GetInstance();
    bool changed = false;
    for (const auto &group : _group_list) {
        if (!group || group->_left_state != 0 || alive_threads.contains(group->_thread_id))
            continue;
        group->_left_state = 3;
        store->SaveGroup(storedFromGroup(*group));
        emit GroupInfoChanged(group->_thread_id);
        changed = true;
    }
    if (changed) {
        emit GroupListChanged();
        emit UnreadChanged();
    }
}

//清除单个会话的历史：会话、游标与最后活动时间都保留，列表顺序不会突然变化
void UserMgr::ClearThreadHistory(qint64 thread_id)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto data = findThreadData(thread_id);
    if (!data)
        return;
    data->_chat_msgs.clear();
    LocalStore::GetInstance()->ClearThreadMessages(thread_id);
    const int peer = FriendIdByThread(thread_id);
    if (auto info = _friend_map.value(peer))
        info->_last_msg.clear();
    emit ConversationChanged(thread_id, peer);
    emit DraftsCleared(thread_id);
}

//清除全部聊天记录：好友、群、会话都在，只是打开后是空的
void UserMgr::ClearAllHistory()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (auto it = _friend_map.begin(); it != _friend_map.end(); ++it) {
        if (!it.value())
            continue;
        it.value()->_chat_thread->_chat_msgs.clear();
        it.value()->_last_msg.clear();
    }
    for (const auto &group : _group_list) {
        if (group)
            group->_chat_thread->_chat_msgs.clear();
    }
    LocalStore::GetInstance()->ClearAllMessages();
    emit FriendListChanged();
    emit GroupListChanged();
    emit DraftsCleared(0);
    //清空全部历史时，窗口里正在显示的那条会话也要刷新；它可能不在前台
    const qint64 open_thread = _window_thread > 0 ? _window_thread : _active_thread;
    if (open_thread > 0)
        emit ConversationChanged(open_thread, FriendIdByThread(open_thread));
}

//媒体缓存被清空后，当前窗口里的图片/视频不能再指向已经不存在的文件
void UserMgr::ForgetActiveResources()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    //用消息窗口所属会话，而不是"正在看"：窗口可能已经失焦或在设置页
    const qint64 open_thread = _window_thread > 0 ? _window_thread : _active_thread;
    if (open_thread <= 0)
        return;
    auto data = findThreadData(open_thread);
    if (!data)
        return;
    for (const auto &message : data->_chat_msgs) {
        if (!message || message->_msg_type == ChatMsgType::Text
            || message->_msg_type == ChatMsgType::System)
            continue;
        message->_imageSource.clear();
        message->_local_path.clear();
        message->_imageWidth = 0;
        message->_imageHeight = 0;
        message->_transferProgress = -1;
        message->_transfer_state = ResourceTransferState::None;
    }
    emit ConversationChanged(open_thread, FriendIdByThread(open_thread));
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
    LocalStore::GetInstance()->EnsureThread(thread_id, static_cast<int>(ChatThreadKind::Private),
                                            friend_id);
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

//按会话找消息容器：群聊查群索引，单聊先按 thread 再按对方 uid 回落
std::shared_ptr<ChatThreadData> UserMgr::findThreadData(qint64 thread_id) const
{
    if (thread_id <= 0)
        return {};
    auto group = _group_map.constFind(thread_id);
    if (group != _group_map.constEnd())
        return (*group)->_chat_thread;
    for (const auto &info : _friend_list) {
        if (info && info->_chat_thread->_thread_id == thread_id)
            return info->_chat_thread;
    }
    for (auto it = _friend_map.cbegin(); it != _friend_map.cend(); ++it) {
        if (it.value() && it.value()->_chat_thread->_thread_id == thread_id)
            return it.value()->_chat_thread;
    }
    return {};
}

bool UserMgr::resolveConversation(const ConversationRef &conv,
                                  std::shared_ptr<ChatThreadData> &data,
                                  ChatThreadKind &kind) const
{
    kind = conv._kind;
    if (conv._kind == ChatThreadKind::Group) {
        auto it = _group_map.constFind(conv._thread_id);
        if (it == _group_map.constEnd() || !(*it))
            return false;
        data = (*it)->_chat_thread;
        return true;
    }

    //单聊优先按 thread_id 找；刚加好友还没建会话时用对方 uid 回落到好友对象
    if (conv._thread_id > 0) {
        auto thread = findThreadData(conv._thread_id);
        if (thread) {
            data = thread;
            return true;
        }
    }
    auto it = _friend_map.constFind(conv._peer_uid);
    if (it == _friend_map.constEnd() || !(*it))
        return false;
    data = (*it)->_chat_thread;
    return true;
}

//发送回执按 msgid 找到原消息，补上服务端 ID 和状态；
//待确认消息原本排在末尾，拿到 ID 后要重新排序才能落到正确位置
void UserMgr::UpdateChatMessage(const ConversationRef &conv, const QString &msg_id,
                                qint64 message_id, ChatMessageStatus status)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return;
    for (const auto &message : thread->_chat_msgs) {
        if (!message || message->_msg_id != msg_id)
            continue;
        if (message_id > 0)
            message->_message_id = message_id;
        if (conv._thread_id > 0)
            message->_thread_id = conv._thread_id;
        message->_status = status;
        thread->AppendMsgs({message});
        //整条重新落库：发消息时会话可能还没有 thread_id，那时库里并没有这一行，
        //只做 UPDATE 会永远补不上；这里按主键合并，存在就改、不存在就插
        const qint64 tid = conv._thread_id > 0 ? conv._thread_id : thread->_thread_id;
        if (tid > 0)
            LocalStore::GetInstance()->SaveMessage(tid, static_cast<int>(kind),
                                                   conv._peer_uid,
                                                   storedFromMessage(message, tid));
        emit ConversationChanged(thread->_thread_id, conv._peer_uid);
        return;
    }
}

//资源上传是异步的：网络线程按资源名回推进度，同一资源可能被多条消息引用，
//所以遍历所有会话一次性更新，再按会话逐个通知模型
void UserMgr::UpdateResourceUploadProgress(const QString &resource_name, qint64 trans, qint64 total)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
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
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
}

void UserMgr::FinishResourceUpload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name
                || message->_status != ChatMessageStatus::Sending)
                continue;
            message->_transferProgress = 100;
            message->_transfer_state = ResourceTransferState::Done;
            message->_status = ChatMessageStatus::Sent;
            changed = true;
        }
        if (changed)
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
}

void UserMgr::FailResourceUpload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name
                || message->_status != ChatMessageStatus::Sending)
                continue;
            message->_transferProgress = -1;
            message->_transfer_state = ResourceTransferState::Failed;
            message->_status = ChatMessageStatus::Failed;
            changed = true;
        }
        if (changed)
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
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
void UserMgr::UpsertResourceMessage(const ConversationRef &conv,
                                    std::shared_ptr<TextChatData> message)
{
    if (!message)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return;

    for (auto &existing : thread->_chat_msgs) {
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
        thread->AppendMsgs({existing}); //拿到服务端 ID 后重新排序
        emit ConversationChanged(thread->_thread_id, conv._peer_uid);
        return;
    }
    AppendChatMsgs(conv, {message});
}

//下载进度只更新还没有完成的资源消息，已经显示出来的图片不再回退
void UserMgr::UpdateResourceDownloadProgress(const QString &resource_name, qint64 current, qint64 total)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
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
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
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
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
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
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
}

void UserMgr::FailResourceDownload(const QString &resource_name)
{
    if (resource_name.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_resource_name != resource_name)
                continue;
            if (message->_transfer_state == ResourceTransferState::Done)
                continue;
            message->_transferProgress = -1;
            message->_transfer_state = ResourceTransferState::Failed;
            changed = true;
        }
        if (changed)
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
}

//视频首帧是独立资源：下载完成后只换预览，视频本体仍等用户点击下载
void UserMgr::SetVideoThumb(const QString &thumb_name, const QString &local_path,
                            const QSize &pixel_size)
{
    if (thumb_name.isEmpty() || local_path.isEmpty())
        return;
    const QString url = QUrl::fromLocalFile(local_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto update = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread)
            return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_msg_type != ChatMsgType::Video
                || message->_thumb_name != thumb_name)
                continue;
            message->_imageSource = url;
            message->_imageWidth = pixel_size.width();
            message->_imageHeight = pixel_size.height();
            changed = true;
        }
        if (changed)
            emit ConversationChanged(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list)
        if (info) update(info->_chat_thread, info->_uid);
    for (const auto &group : _group_list)
        if (group) update(group->_chat_thread, 0);
}

void UserMgr::SetResourceState(qint64 message_id, ResourceTransferState state, int progress)
{
    if (message_id <= 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        if (!info) continue;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (!message || message->_message_id != message_id)
                continue;
            message->_transfer_state = state;
            message->_transferProgress = progress;
            emit ConversationChanged(info->_chat_thread->_thread_id, info->_uid);
            return;
        }
    }
    for (const auto &group : _group_list) {
        if (!group) continue;
        for (const auto &message : group->_chat_thread->_chat_msgs) {
            if (!message || message->_message_id != message_id)
                continue;
            message->_transfer_state = state;
            message->_transferProgress = progress;
            emit ConversationChanged(group->_thread_id, 0);
            return;
        }
    }
}

void UserMgr::SetResourceStateByUniqueId(const ConversationRef &conv, const QString &msg_id,
                                         ResourceTransferState state, int progress)
{
    if (msg_id.isEmpty())
        return;
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return;
    for (const auto &message : thread->_chat_msgs) {
        if (!message || message->_msg_id != msg_id)
            continue;
        message->_transfer_state = state;
        message->_transferProgress = progress;
        emit ConversationChanged(thread->_thread_id, conv._peer_uid);
        return;
    }
}

void UserMgr::SetLocalVideoThumb(const ConversationRef &conv, const QString &msg_id,
                                 const QString &local_path, const QSize &pixel_size)
{
    if (msg_id.isEmpty() || local_path.isEmpty())
        return;
    const QString url = QUrl::fromLocalFile(local_path).toString();
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return;
    for (const auto &message : thread->_chat_msgs) {
        if (!message || message->_msg_id != msg_id)
            continue;
        message->_imageSource = url;
        message->_imageWidth = pixel_size.width();
        message->_imageHeight = pixel_size.height();
        emit ConversationChanged(thread->_thread_id, conv._peer_uid);
        return;
    }
}

std::shared_ptr<TextChatData> UserMgr::FindMessage(qint64 message_id, qint64 *thread_id) const
{
    if (message_id <= 0)
        return {};
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        if (!info) continue;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (message && message->_message_id == message_id) {
                if (thread_id)
                    *thread_id = info->_chat_thread->_thread_id;
                return message;
            }
        }
    }
    for (const auto &group : _group_list) {
        if (!group) continue;
        for (const auto &message : group->_chat_thread->_chat_msgs) {
            if (message && message->_message_id == message_id) {
                if (thread_id)
                    *thread_id = group->_thread_id;
                return message;
            }
        }
    }
    return {};
}

std::shared_ptr<TextChatData> UserMgr::FindMessageByUniqueId(const QString &msg_id,
                                                             qint64 *thread_id) const
{
    if (msg_id.isEmpty())
        return {};
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    for (const auto &info : _friend_list) {
        if (!info) continue;
        for (const auto &message : info->_chat_thread->_chat_msgs) {
            if (message && message->_msg_id == msg_id) {
                if (thread_id)
                    *thread_id = info->_chat_thread->_thread_id;
                return message;
            }
        }
    }
    for (const auto &group : _group_list) {
        if (!group) continue;
        for (const auto &message : group->_chat_thread->_chat_msgs) {
            if (message && message->_msg_id == msg_id) {
                if (thread_id)
                    *thread_id = group->_thread_id;
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
    auto touch = [&](const std::shared_ptr<ChatThreadData> &thread, int peer) {
        if (!thread) return false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_message_id != message_id)
                continue;
            message->_local_path.clear();
            message->_transfer_state = ResourceTransferState::None;
            message->_transferProgress = -1;
            emit ConversationChanged(thread->_thread_id, peer);
            return true;
        }
        return false;
    };
    for (const auto &info : _friend_list)
        if (info && touch(info->_chat_thread, info->_uid)) return;
    for (const auto &group : _group_list)
        if (group && touch(group->_chat_thread, 0)) return;
}

//断线时把所有还在「发送中」的消息标失败，避免界面一直转圈
void UserMgr::FailPendingMessages()
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto fail = [](const std::shared_ptr<ChatThreadData> &thread, int peer,
                   std::function<void(qint64, int)> notify) {
        if (!thread) return;
        bool changed = false;
        for (const auto &message : thread->_chat_msgs) {
            if (!message || message->_status != ChatMessageStatus::Sending)
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
            notify(thread->_thread_id, peer);
    };
    for (const auto &info : _friend_list) {
        if (!info) continue;
        fail(info->_chat_thread, info->_uid, [this](qint64 thread, int peer) {
            emit ConversationChanged(thread, peer);
        });
    }
    for (const auto &group : _group_list) {
        if (!group) continue;
        fail(group->_chat_thread, 0, [this](qint64 thread, int peer) {
            emit ConversationChanged(thread, peer);
        });
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
        if (info && info->_chat_thread->_thread_id == thread_id)
            return info->_uid;
    for (auto it = _friend_map.cbegin(); it != _friend_map.cend(); ++it) {
        if (it.value() && it.value()->_chat_thread->_thread_id == thread_id)
            return it.key();
    }
    return 0;
}

std::shared_ptr<ChatThreadData> UserMgr::ThreadData(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return findThreadData(thread_id);
}

ChatThreadKind UserMgr::ThreadKind(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _group_map.contains(thread_id) ? ChatThreadKind::Group : ChatThreadKind::Private;
}

int UserMgr::ThreadPeer(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return FriendIdByThread(thread_id);
}

QString UserMgr::ThreadTitle(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.constFind(thread_id);
    if (group != _group_map.constEnd() && (*group))
        return (*group)->_name;
    const int peer = FriendIdByThread(thread_id);
    auto info = peer > 0 ? _friend_map.value(peer) : nullptr;
    if (!info)
        return {};
    return DisplayName(info->_back, info->_nick, info->_name);
}

bool UserMgr::ThreadMuted(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    //群免打扰以服务端 grp.muted 为准；单聊用 thread 上的本地标记
    auto group = _group_map.constFind(thread_id);
    if (group != _group_map.constEnd() && (*group))
        return (*group)->_muted;
    auto data = findThreadData(thread_id);
    return data && data->_muted;
}

int UserMgr::ThreadMemberCount(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.constFind(thread_id);
    return group == _group_map.constEnd() || !(*group) ? 0 : (*group)->_member_count;
}

//历史消息按最大 message_id 继续翻页；待确认消息 ID 为 0，不参与游标
qint64 UserMgr::LastMessageId(const ConversationRef &conv) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    std::shared_ptr<ChatThreadData> thread;
    ChatThreadKind kind = ChatThreadKind::Private;
    if (!resolveConversation(conv, thread, kind) || !thread)
        return 0;
    qint64 lastId = 0;
    for (const auto &message : thread->_chat_msgs)
        lastId = qMax(lastId, message->_message_id);
    return lastId;
}

// ---- 群聊 ----

void UserMgr::UpsertGroupThread(const QJsonObject &item)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    const qint64 thread_id = item.value("thread_id").toVariant().toLongLong();
    if (thread_id <= 0)
        return;

    auto group = _group_map.value(thread_id);
    const bool created = !group;
    if (created) {
        group = std::make_shared<GroupInfo>(thread_id);
        _group_map[thread_id] = group;
        _group_list.push_back(group);
    }
    group->_name = item.value("group_name").toString();
    if (group->_name.isEmpty())
        group->_name = QStringLiteral("群聊");
    group->_owner_id = item.value("owner").toInt();
    group->_member_count = item.value("member_count").toInt();
    group->_my_role = item.value("my_role").toInt();
    //会话列表里出现且带我的角色，说明我又是成员了（被重新拉回群）：离开标记要清掉
    if (item.contains("my_role"))
        group->_left_state = 0;
    group->_muted = item.value("muted").toBool();
    //服务端会话列表暂不下发 at_me：字段缺省时保留本地记录，
    //否则每次登录都会把"[有人@我]"清掉，未读还在但提醒没了
    if (item.contains("at_me"))
        group->_at_me = item.value("at_me").toBool();
    group->_last_time = item.value("time").toVariant().toLongLong();
    group->_chat_thread->_thread_id = thread_id;

    const int unread = item.value("unread").toInt(0);
    if (unread > 0)
        _unread[thread_id] = unread;
    else
        _unread.remove(thread_id);
    if (group->_at_me)
        _at_me_threads.insert(thread_id);
    else
        _at_me_threads.remove(thread_id);

    auto store = LocalStore::GetInstance();
    store->SaveGroup(storedFromGroup(*group));
    store->SaveThreadTime(thread_id, group->_last_time);
    store->SaveThreadUnread(thread_id, unread, group->_at_me);
    if (created)
        emit GroupListChanged();
    emit UnreadChanged();
}

//群资料：名称、公告、我的角色与免打扰。5004 与 5009 info 共用这一条路径
void UserMgr::ApplyGroupProfile(const QJsonObject &group_json)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    const qint64 thread_id = group_json.value("thread_id").toVariant().toLongLong();
    auto group = _group_map.value(thread_id);
    if (!group) {
        //收到未知群的资料（刚被邀请）：先建对象，成员列表由后续 5004 补齐
        group = std::make_shared<GroupInfo>(thread_id);
        _group_map[thread_id] = group;
        _group_list.push_back(group);
        emit GroupListChanged();
    }
    if (group_json.contains("name"))
        group->_name = group_json.value("name").toString();
    if (group_json.contains("notice"))
        group->_notice = group_json.value("notice").toString();
    if (group_json.contains("owner"))
        group->_owner_id = group_json.value("owner").toInt();
    if (group_json.contains("member_count"))
        group->_member_count = group_json.value("member_count").toInt();
    if (group_json.contains("my_role")) {
        group->_my_role = group_json.value("my_role").toInt();
        group->_left_state = 0; //重新入群后资料能拉到，说明成员身份已恢复
    }
    if (group_json.contains("muted"))
        group->_muted = group_json.value("muted").toBool();
    if (group_json.contains("my_nick"))
        group->_my_nick = group_json.value("my_nick").toString();
    if (group_json.contains("created_at"))
        group->_created_at = group_json.value("created_at").toVariant().toLongLong();
    group->_chat_thread->_thread_id = thread_id;
    LocalStore::GetInstance()->SaveGroup(storedFromGroup(*group));
    emit GroupInfoChanged(thread_id);
}

void UserMgr::SetGroupMembers(qint64 thread_id, const QJsonArray &members)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group)
        return;
    group->_member_map.clear();
    for (const auto &value : members) {
        const QJsonObject item = value.toObject();
        const int uid = item.value("uid").toInt();
        if (uid <= 0)
            continue;
        group->_member_map.insert(uid, GroupMember(uid, item.value("name").toString(),
            item.value("nick").toString(), item.value("icon").toString(),
            item.value("sex").toInt(), item.value("role").toInt(),
            item.value("group_nick").toString(),
            item.value("joined_at").toVariant().toLongLong()));
    }
    if (group->_member_count <= 0)
        group->_member_count = group->_member_map.size();
    QVector<StoredMember> stored;
    stored.reserve(group->_member_map.size());
    for (auto it = group->_member_map.cbegin(); it != group->_member_map.cend(); ++it)
        stored.push_back(storedFromMember(thread_id, it.value()));
    LocalStore::GetInstance()->SaveMembers(thread_id, stored);
    emit GroupInfoChanged(thread_id);
}

void UserMgr::AddGroupMember(qint64 thread_id, const GroupMember &member)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group || member._uid <= 0)
        return;
    group->_member_map.insert(member._uid, member);
    LocalStore::GetInstance()->SaveMember(storedFromMember(thread_id, member));
    emit GroupInfoChanged(thread_id);
}

void UserMgr::SetGroupMemberCount(qint64 thread_id, int count)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group || count < 0)
        return;
    group->_member_count = count;
    LocalStore::GetInstance()->SaveGroup(storedFromGroup(*group));
    emit GroupInfoChanged(thread_id);
}


std::shared_ptr<GroupInfo> UserMgr::GetGroup(qint64 thread_id) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _group_map.value(thread_id);
}

int UserMgr::GroupCount() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return static_cast<int>(_group_list.size());
}

std::shared_ptr<GroupInfo> UserMgr::GroupAt(int index) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (index < 0 || index >= static_cast<int>(_group_list.size()))
        return nullptr;
    return _group_list[index];
}

//群消息里的发送者显示名：群昵称 > 好友备注 > 昵称 > 用户名。
//成员已退群时回落到"群成员"，不能把别人的私聊备注带进群
QString UserMgr::GroupSenderName(qint64 thread_id, int uid) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group)
        return QString::number(uid);
    const GroupMember *member = group->Member(uid);
    if (!member) {
        //成员表还没拉回来时先用好友备注/昵称，至少不会把熟人显示成"群成员"
        auto friend_info = _friend_map.value(uid);
        if (friend_info)
            return DisplayName(friend_info->_back, friend_info->_nick, friend_info->_name);
        return QStringLiteral("群成员");
    }
    auto friend_info = _friend_map.value(uid);
    const QString remark = friend_info ? friend_info->_back : QString();
    if (!member->_group_nick.isEmpty())
        return member->_group_nick;
    return DisplayName(remark, member->_nick, member->_name);
}

QString UserMgr::GroupSenderIcon(qint64 thread_id, int uid) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    //好友头像有本机缓存，优先用它；不是好友时再用成员资料里的服务端字段
    auto friend_info = _friend_map.value(uid);
    if (friend_info && !friend_info->_icon.isEmpty())
        return friend_info->_icon;
    auto group = _group_map.value(thread_id);
    if (!group)
        return {};
    const GroupMember *member = group->Member(uid);
    return member ? member->_icon : QString();
}

void UserMgr::SetGroupMuted(qint64 thread_id, bool muted)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto group = _group_map.value(thread_id);
    if (!group || group->_muted == muted)
        return;
    group->_muted = muted;
    auto store = LocalStore::GetInstance();
    store->SaveGroup(storedFromGroup(*group));
    store->SaveThreadUnread(thread_id, _unread.value(thread_id, 0),
                            _at_me_threads.contains(thread_id));
    emit GroupInfoChanged(thread_id);
    emit GroupListChanged();
    emit UnreadChanged(); //角标口径与免打扰有关，开关后立即重算
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

//会话对象包含已删好友但仍有历史的人：会话列表不能因为关系没了就把记录藏起来
int UserMgr::PeerCount() const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    return _friend_map.size();
}

std::shared_ptr<FriendInfo> UserMgr::PeerAt(int index) const
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    if (index < 0 || index >= _friend_map.size())
        return nullptr;
    auto it = _friend_map.cbegin();
    std::advance(it, index);
    return it.value();
}

//删好友只动联系人列表：对象留在 _friend_map 里，会话与历史继续可用
void UserMgr::RemoveFriend(int uid)
{
    std::lock_guard<std::recursive_mutex> lock(_mtx);
    auto find_it = _friend_map.find(uid);
    if (find_it == _friend_map.end() || !(*find_it))
        return;

    emit FriendListAboutToChange();
    (*find_it)->_is_friend = false;
    _friend_list.erase(std::remove_if(_friend_list.begin(), _friend_list.end(),
        [uid](const auto &info) { return !info || info->_uid == uid; }), _friend_list.end());
    //只置标志、不删行：历史会话与库里已经同步下来的消息都还要用这个 uid 渲染
    LocalStore::GetInstance()->SetFriendActive(uid, false);
    emit FriendListChanged();
    emit FriendTagsChanged(); //标签筛选条要按新的好友集合重算
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
    //先关库再清内存：换账号时旧连接必须释放，否则下一次 Open 会沿用上一个账号的库
    LocalStore::GetInstance()->Close();
    const bool hadIcon = !GetIcon().isEmpty();
    emit ApplyListAboutToChange();
    emit FriendListAboutToChange();
    _user_info.reset();
    _token.clear();
    _localIcon.clear();
    _apply_list.clear();
    _friend_list.clear();
    _friend_map.clear();
    _group_list.clear();
    _group_map.clear();
    _pending_private_chats.clear();
    _uploads.clear();
    _downloads.clear();
    _upload_paused = false;

    emit ApplyListChanged();
    emit FriendListChanged();
    emit GroupListChanged();
    emit FriendTagsChanged(); //换账号后筛选条不能留着上一个账号的标签
    _unread.clear(); //未读属于上一次登录，换账号必须清掉
    _at_me_threads.clear();
    _active_thread = 0;
    _window_thread = 0;
    emit UnreadChanged();
    if (hadIcon)
        emit UserIconChanged();
}
