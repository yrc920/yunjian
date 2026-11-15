#include "conversationmodel.h"

#include <algorithm>

#include "timeformat.h"
#include "userdata.h"
#include "usermgr.h"

namespace {
//会话预览里的类型占位符，与单聊保持一致
QString typePreview(const std::shared_ptr<TextChatData> &message)
{
    if (!message)
        return {};
    if (message->_recalled)
        return QStringLiteral("撤回了一条消息");
    switch (message->_msg_type) {
    case ChatMsgType::Image:
        return QStringLiteral("[图片]");
    case ChatMsgType::Video:
        return QStringLiteral("[视频]");
    case ChatMsgType::File:
        return message->_file_name.isEmpty()
            ? QStringLiteral("[文件]")
            : QStringLiteral("[文件] ") + message->_file_name;
    case ChatMsgType::System:
        return message->_msg_content;
    default:
        return message->_msg_content;
    }
}
}

ConversationModel::ConversationModel(QObject *parent) : QAbstractListModel(parent)
{
    auto *mgr = UserMgr::GetInstance().get();
    //好友与群的变化都可能在彼此的通知里发生（登录先写好友再补群），
    //统一在"已变化"之后自己开合 reset，避免嵌套 beginResetModel
    connect(mgr, &UserMgr::FriendListChanged, this, [this] {
        beginResetModel();
        _rows = computeRows();
        endResetModel();
        emit countChanged();
    });
    connect(mgr, &UserMgr::GroupListChanged, this, [this] {
        beginResetModel();
        _rows = computeRows();
        endResetModel();
        emit countChanged();
    });
    //消息变化可能改变排序：先算新顺序，顺序没变才只刷新这一行
    connect(mgr, &UserMgr::ConversationChanged, this,
            [this](qint64 thread_id, int peer_uid) {
        Row target{peer_uid > 0 ? 1 : 2, peer_uid, thread_id};
        if (target.kind == 1 && target.thread == 0) {
            const int row = indexOfRow(target);
            if (row >= 0)
                target = _rows[row];
        }
        QVector<Row> next = computeRows();
        if (next.size() == _rows.size()) {
            bool same = true;
            for (int i = 0; i < next.size(); ++i) {
                if (next[i].kind != _rows[i].kind || next[i].uid != _rows[i].uid
                    || next[i].thread != _rows[i].thread) {
                    same = false;
                    break;
                }
            }
            if (same) {
                const int row = indexOfRow(target);
                if (row >= 0) {
                    const auto idx = index(row, 0);
                    emit dataChanged(idx, idx, {LastTextRole, LastTimeRole});
                }
                return;
            }
        }
        beginResetModel();
        _rows = std::move(next);
        endResetModel();
        emit countChanged();
    });
    connect(mgr, &UserMgr::GroupInfoChanged, this, [this](qint64 thread_id) {
        const int row = indexOfThreadInternal(thread_id);
        if (row < 0)
            return;
        const auto idx = index(row, 0);
        //成员表更新后，群消息预览里的发送者名字也要重算
        emit dataChanged(idx, idx, {DisplayNameRole, InitialRole, MemberCountRole,
                                    SubtitleRole, MutedRole, LastTextRole});
    });
    connect(mgr, &UserMgr::UnreadChanged, this, [this] {
        if (_rows.isEmpty())
            return;
        emit dataChanged(index(0, 0), index(_rows.size() - 1, 0), {UnreadRole, AtMeRole});
    });
    connect(mgr, &UserMgr::FriendIconChanged, this, [this](int uid) {
        for (int row = 0; row < _rows.size(); ++row) {
            if (_rows[row].kind == 1 && _rows[row].uid == uid) {
                const auto idx = index(row, 0);
                emit dataChanged(idx, idx, {AvatarRole});
            }
        }
    });
    connect(mgr, &UserMgr::FriendRelationChanged, this, [this](int uid) {
        for (int row = 0; row < _rows.size(); ++row) {
            if (_rows[row].kind == 1 && _rows[row].uid == uid) {
                const auto idx = index(row, 0);
                emit dataChanged(idx, idx, {DisplayNameRole, InitialRole, LastTextRole});
            }
        }
    });
    connect(mgr, &UserMgr::FriendProfileChanged, this, [this](int uid) {
        for (int row = 0; row < _rows.size(); ++row) {
            if (_rows[row].kind == 1 && _rows[row].uid == uid) {
                const auto idx = index(row, 0);
                emit dataChanged(idx, idx, {DisplayNameRole, InitialRole});
            }
        }
    });
    //登录或刷新后先建一次行视图，避免首屏空白
    _rows = computeRows();
}

QVector<ConversationModel::Row> ConversationModel::computeRows() const
{
    auto *mgr = UserMgr::GetInstance().get();
    QVector<Row> rows;
    rows.reserve(mgr->PeerCount() + mgr->GroupCount());
    for (int i = 0; i < mgr->PeerCount(); ++i) {
        auto info = mgr->PeerAt(i);
        if (!info)
            continue;
        const qint64 thread = info->_chat_thread ? info->_chat_thread->_thread_id : 0;
        //还没建立会话的好友只在联系人里出现；打开会话（1025）后才进这张表
        if (thread <= 0)
            continue;
        Row row;
        row.kind = 1;
        row.uid = info->_uid;
        row.thread = thread;
        rows.push_back(row);
    }
    for (int i = 0; i < mgr->GroupCount(); ++i) {
        auto group = mgr->GroupAt(i);
        if (!group)
            continue;
        Row row;
        row.kind = 2;
        row.thread = group->_thread_id;
        rows.push_back(row);
    }
    //没有消息的会话排在后面；stable_sort 让同时间的行保持原有顺序，列表不会来回跳
    std::stable_sort(rows.begin(), rows.end(), [this](const Row &left, const Row &right) {
        return lastMessageTime(left) > lastMessageTime(right);
    });
    return rows;
}

qint64 ConversationModel::lastMessageTime(const std::shared_ptr<FriendInfo> &info) const
{
    if (!info)
        return 0;
    if (info->_chat_thread) {
        const auto &msgs = info->_chat_thread->_chat_msgs;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (*it && (*it)->_time > 0)
                return (*it)->_time;
        }
    }
    return info->_last_time;
}

qint64 ConversationModel::lastMessageTime(const std::shared_ptr<GroupInfo> &info) const
{
    if (!info)
        return 0;
    if (info->_chat_thread) {
        const auto &msgs = info->_chat_thread->_chat_msgs;
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (*it && (*it)->_time > 0)
                return (*it)->_time;
        }
    }
    return info->_last_time;
}

qint64 ConversationModel::lastMessageTime(const Row &row) const
{
    auto *mgr = UserMgr::GetInstance().get();
    if (row.kind == 2)
        return lastMessageTime(mgr->GetGroup(row.thread));
    return lastMessageTime(mgr->GetFriendById(row.uid));
}

QString ConversationModel::lastText(const std::shared_ptr<FriendInfo> &info) const
{
    if (!info)
        return {};
    if (!info->_chat_thread->_chat_msgs.empty()) {
        const auto &last = info->_chat_thread->_chat_msgs.back();
        if (last)
            return typePreview(last);
    }
    return info->_last_msg;
}

//群聊预览带发送者前缀，和微信一致：不点进去也能看出是谁在说话
QString ConversationModel::lastText(const std::shared_ptr<GroupInfo> &info) const
{
    if (!info)
        return {};
    if (info->_chat_thread->_chat_msgs.empty())
        return QString();
    const auto &last = info->_chat_thread->_chat_msgs.back();
    if (!last)
        return {};
    const QString body = typePreview(last);
    if (last->_msg_type == ChatMsgType::System)
        return body;
    auto *mgr = UserMgr::GetInstance().get();
    const QString sender = last->_from_uid == mgr->GetUid()
        ? QStringLiteral("我")
        : mgr->GroupSenderName(info->_thread_id, last->_from_uid);
    return sender + QStringLiteral(": ") + body;
}

int ConversationModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return _rows.size();
}

int ConversationModel::activeCount() const
{
    int count = 0;
    for (const auto &row : _rows) {
        if (row.kind == 2 || row.thread > 0)
            ++count;
    }
    return count;
}

QVariant ConversationModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= _rows.size())
        return {};
    const Row &row = _rows[index.row()];
    auto *mgr = UserMgr::GetInstance().get();

    if (role == KindRole)
        return row.kind;
    if (role == ThreadIdRole)
        return QVariant::fromValue(row.thread);

    if (row.kind == 2) {
        auto group = mgr->GetGroup(row.thread);
        if (!group)
            return {};
        switch (role) {
        case UidRole: return 0;
        case DisplayNameRole: return group->_name;
        case AvatarRole: return QString();
        case InitialRole: return group->_name.isEmpty() ? QStringLiteral("#")
                                                       : group->_name.left(1).toUpper();
        case LastTextRole: return lastText(group);
        case LastTimeRole: return TimeFormat::listTime(lastMessageTime(group));
        case UnreadRole: return mgr->ThreadUnread(row.thread);
        case MutedRole: return group->_muted;
        case AtMeRole: return mgr->ThreadAtMe(row.thread);
        case MemberCountRole: return group->_member_count;
        case SubtitleRole:
            return group->_member_count > 0
                ? QStringLiteral("%1 人").arg(group->_member_count) : QString();
        default: return {};
        }
    }

    auto info = mgr->GetFriendById(row.uid);
    if (!info)
        return {};
    switch (role) {
    case UidRole: return info->_uid;
    case DisplayNameRole: return DisplayName(info->_back, info->_nick, info->_name);
    case AvatarRole: return info->_icon;
    case InitialRole: {
        const QString name = DisplayName(info->_back, info->_nick, info->_name);
        return name.isEmpty() ? QStringLiteral("#") : name.left(1).toUpper();
    }
    case LastTextRole: return lastText(info);
    case LastTimeRole: return TimeFormat::listTime(lastMessageTime(info));
    case UnreadRole: return mgr->ThreadUnread(row.thread);
    case MutedRole: return false;
    case AtMeRole: return false;
    case MemberCountRole: return 0;
    case SubtitleRole: return QString();
    default: return {};
    }
}

QHash<int, QByteArray> ConversationModel::roleNames() const
{
    return {
        {KindRole, "kind"},
        {ThreadIdRole, "threadId"},
        {UidRole, "uid"},
        {DisplayNameRole, "displayName"},
        {AvatarRole, "icon"},
        {InitialRole, "initial"},
        {LastTextRole, "lastText"},
        {LastTimeRole, "lastTime"},
        {UnreadRole, "unread"},
        {MutedRole, "muted"},
        {AtMeRole, "atMe"},
        {MemberCountRole, "memberCount"},
        {SubtitleRole, "subtitle"},
    };
}

int ConversationModel::indexOfRow(const Row &row) const
{
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].kind != row.kind)
            continue;
        if (row.kind == 2 && _rows[i].thread == row.thread)
            return i;
        if (row.kind == 1 && _rows[i].uid == row.uid)
            return i;
    }
    return -1;
}

int ConversationModel::indexOfThreadInternal(qint64 thread_id) const
{
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].kind == 2 && _rows[i].thread == thread_id)
            return i;
    }
    return -1;
}

int ConversationModel::indexOfThread(qint64 thread_id) const
{
    const int row = indexOfThreadInternal(thread_id);
    if (row >= 0)
        return row;
    for (int i = 0; i < _rows.size(); ++i) {
        if (_rows[i].thread == thread_id)
            return i;
    }
    return -1;
}

void ConversationModel::refreshRow(int row)
{
    if (row < 0 || row >= _rows.size())
        return;
    const auto idx = index(row, 0);
    emit dataChanged(idx, idx);
}

QVariantMap ConversationModel::get(int row) const
{
    const QModelIndex idx = index(row, 0);
    QVariantMap map;
    //roleNames() 每次返回一个新容器，必须取一份再遍历，不能同时用两个临时对象
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}
