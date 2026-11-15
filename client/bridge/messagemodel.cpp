#include "messagemodel.h"

#include <ctime>

#include "usermgr.h"
#include "userdata.h"
#include "timeformat.h"

MessageModel::MessageModel(QObject *parent) : QAbstractListModel(parent)
{
    auto *mgr = UserMgr::GetInstance().get();
    connect(mgr, &UserMgr::ConversationChanged, this, &MessageModel::onConversationChanged);
    //头像/资料变化只刷新当前会话里相关的行，不重建列表
    connect(mgr, &UserMgr::UserIconChanged, this, [this] {
        for (int row = 0; row < _cachedCount; ++row)
            emit dataChanged(index(row, 0), index(row, 0), {AvatarRole});
    });
    connect(mgr, &UserMgr::FriendIconChanged, this, [this](int uid) {
        for (int row = 0; row < _cachedCount; ++row) {
            if (data(index(row, 0), FromUidRole).toInt() == uid)
                emit dataChanged(index(row, 0), index(row, 0), {AvatarRole});
        }
    });
    //备注、昵称或群昵称变化后，气泡上的名字按新的显示名重绘
    auto refreshNames = [this] {
        for (int row = 0; row < _cachedCount; ++row)
            emit dataChanged(index(row, 0), index(row, 0), {NameRole, SenderNameRole});
    };
    connect(mgr, &UserMgr::FriendRelationChanged, this, [this, refreshNames](int uid) {
        if (_kind == static_cast<int>(ChatThreadKind::Private) && uid == _peerUid)
            refreshNames();
    });
    connect(mgr, &UserMgr::FriendProfileChanged, this, [this, refreshNames](int uid) {
        if (_kind == static_cast<int>(ChatThreadKind::Private) && uid == _peerUid)
            refreshNames();
    });
    connect(mgr, &UserMgr::UserProfileChanged, this, refreshNames);
    connect(mgr, &UserMgr::GroupInfoChanged, this, [this, refreshNames](qint64 thread_id) {
        if (thread_id == _threadId)
            refreshNames();
    });
}

// 切换当前会话：同一个会话重复调用直接返回，否则点一次当前会话就会重置模型
void MessageModel::setConversation(qint64 thread_id, int kind, int peer_uid)
{
    if (thread_id == _threadId && kind == _kind && peer_uid == _peerUid
        && (thread_id != 0 || peer_uid != 0))
        return;
    beginResetModel();
    _threadId = thread_id;
    _kind = kind;
    _peerUid = peer_uid;
    auto data = threadData();
    if (data)
        syncCache(data->_chat_msgs);
    else {
        _cachedCount = 0;
        _cachedIds.clear();
        _cachedStatus.clear();
    }
    endResetModel();
    emit countChanged();
}

std::shared_ptr<ChatThreadData> MessageModel::threadData() const
{
    auto *mgr = UserMgr::GetInstance().get();
    if (_kind == static_cast<int>(ChatThreadKind::Group))
        return mgr->ThreadData(_threadId);
    if (_threadId > 0) {
        if (auto data = mgr->ThreadData(_threadId))
            return data;
    }
    auto info = mgr->GetFriendById(_peerUid);
    return info ? info->_chat_thread : nullptr;
}

// 记录当前行顺序和状态，供下一次变更做前缀比较
void MessageModel::syncCache(const std::vector<std::shared_ptr<TextChatData>>& msgs)
{
    _cachedCount = static_cast<int>(msgs.size());
    _cachedIds.resize(_cachedCount);
    _cachedStatus.resize(_cachedCount);
    for (int i = 0; i < _cachedCount; ++i) {
        if (!msgs[i]) {
            _cachedIds[i].clear();
            _cachedStatus[i] = 0;
            continue;
        }
        _cachedIds[i] = msgs[i]->_msg_id;
        _cachedStatus[i] = static_cast<int>(msgs[i]->_status);
    }
}

int MessageModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return _cachedCount;
}

// 按角色取字段；isSelf 决定气泡左右，群聊还要给出送者名字与头像
QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= rowCount())
        return {};
    auto thread = threadData();
    if (!thread || index.row() >= static_cast<int>(thread->_chat_msgs.size()))
        return {};

    auto *mgr = UserMgr::GetInstance().get();
    const int self_uid = mgr->GetUid();
    const auto &msg = thread->_chat_msgs.at(index.row());
    if (!msg)
        return {};

    switch (role) {
    case TextRole: return msg->_msg_content;
    case TimeTextRole: {
        //返回空串而不是无效 QVariant：QML 里 model.timeText.length 不能读 undefined
        if (msg->_time <= 0)
            return QString();
        //与上一条间隔不到 5 分钟就沿用上一条的时间条，避免每句都挂一个时间
        if (index.row() > 0) {
            const auto &prev = thread->_chat_msgs.at(index.row() - 1);
            if (prev && prev->_time > 0
                && msg->_time - prev->_time < TimeFormat::kSeparatorSeconds)
                return QString();
        }
        return TimeFormat::messageTime(msg->_time);
    }
    case RecalledRole:
        return msg->_recalled;
    case CanRecallRole: {
        if (msg->_recalled || msg->_from_uid != self_uid)
            return false;
        if (msg->_message_id <= 0 || msg->_time <= 0)
            return false;
        if (msg->_status == ChatMessageStatus::Failed)
            return false;
        return std::time(nullptr) - msg->_time <= kRecallWindowSeconds;
    }
    case StatusTextRole: {
        //群里不做逐条已读，只有单聊才给"已读/未读"结论
        if (_kind != static_cast<int>(ChatThreadKind::Private))
            return QString();
        //只有"自己发的最后一条"值得给结论：一屏十几条都挂状态，噪声大于信息
        if (msg->_recalled || msg->_from_uid != self_uid)
            return QString();
        if (msg->_status != ChatMessageStatus::Sent && msg->_status != ChatMessageStatus::Read)
            return QString();
        for (int i = static_cast<int>(thread->_chat_msgs.size()) - 1; i >= 0; --i) {
            const auto &item = thread->_chat_msgs[i];
            if (!item || item->_from_uid != self_uid)
                continue;
            if (i != index.row())
                return QString();
            break;
        }
        return msg->_status == ChatMessageStatus::Read
            ? QStringLiteral("已读") : QStringLiteral("未读");
    }
    case CanRetryRole:
        return msg->_status == ChatMessageStatus::Failed && !msg->_recalled;
    case RecallTextRole:
        //只有自己撤回的文本才有可编辑的原文
        return msg->_recalled && msg->_from_uid == self_uid
            ? msg->_recalled_text : QString();
    case SelfRole: return msg->_from_uid == self_uid;
    case FromUidRole: return msg->_from_uid;
    case ImageSourceRole: return msg->_imageSource;
    case ImageWidthRole: return msg->_imageWidth;
    case ImageHeightRole: return msg->_imageHeight;
    case AvatarRole: {
        if (msg->_from_uid == self_uid)
            return mgr->GetIcon();
        if (_kind == static_cast<int>(ChatThreadKind::Group))
            return mgr->GroupSenderIcon(_threadId, msg->_from_uid);
        auto friend_info = mgr->GetFriendById(_peerUid);
        return friend_info ? friend_info->_icon : QString();
    }
    case NameRole:
    case SenderNameRole: {
        if (msg->_from_uid == self_uid) {
            auto self = mgr->GetUserInfo();
            if (!self)
                return QString();
            return DisplayName(QString(), self->_nick, self->_name);
        }
        if (_kind == static_cast<int>(ChatThreadKind::Group))
            return mgr->GroupSenderName(_threadId, msg->_from_uid);
        auto friend_info = mgr->GetFriendById(_peerUid);
        return friend_info ? DisplayName(friend_info->_back, friend_info->_nick,
                                         friend_info->_name) : QString();
    }
    case StatusRole:
        return static_cast<int>(msg->_status);
    case IsImageRole:
        return msg->_msg_type == ChatMsgType::Image;
    case TransferProgressRole:
        return msg->_transferProgress;
    case MessageIdRole:
        return QVariant::fromValue(msg->_message_id);
    case MsgIdRole:
        return msg->_msg_id;
    case MsgTypeRole:
        return static_cast<int>(msg->_msg_type);
    case FileNameRole:
        return msg->_file_name;
    case FileSizeRole:
        return QVariant::fromValue(msg->_file_size);
    case TransferStateRole:
        return static_cast<int>(msg->_transfer_state);
    case ShowSenderRole:
        return _kind == static_cast<int>(ChatThreadKind::Group)
            && msg->_from_uid != self_uid && msg->_msg_type != ChatMsgType::System;
    case SystemRole:
        return msg->_msg_type == ChatMsgType::System;
    case MentionedRole:
        return msg->_mentions.contains(QString::number(self_uid));
    case KindRole:
        return _kind;
    default: return {};
    }
}

// 暴露给 QML 的角色名
QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {
        {TextRole, "text"},
        {SelfRole, "isSelf"},
        {FromUidRole, "fromUid"},
        {ImageSourceRole, "imageSource"},
        {ImageWidthRole, "imageWidth"},
        {ImageHeightRole, "imageHeight"},
        {AvatarRole, "avatar"},
        {NameRole, "name"},
        {StatusRole, "status"},
        {IsImageRole, "isImage"},
        {TransferProgressRole, "transferProgress"},
        {MessageIdRole, "messageId"},
        {MsgIdRole, "msgId"},
        {MsgTypeRole, "msgType"},
        {FileNameRole, "fileName"},
        {FileSizeRole, "fileSize"},
        {TransferStateRole, "transferState"},
        {TimeTextRole, "timeText"},
        {RecalledRole, "recalled"},
        {CanRecallRole, "canRecall"},
        {StatusTextRole, "statusText"},
        {CanRetryRole, "canRetry"},
        {RecallTextRole, "recallText"},
        {SenderNameRole, "senderName"},
        {ShowSenderRole, "showSender"},
        {SystemRole, "isSystem"},
        {MentionedRole, "mentioned"},
        {KindRole, "threadKind"},
    };
}

QVariantMap MessageModel::get(int row) const
{
    const QModelIndex idx = index(row, 0);
    QVariantMap map;
    //roleNames() 每次返回一个新容器，必须取一份再遍历，不能同时用两个临时对象
    const auto roles = roleNames();
    for (auto it = roles.cbegin(); it != roles.cend(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}

// 当前会话消息变化：顺序不变时按增量插入或按行刷新，只有重排/缩表才 reset
void MessageModel::onConversationChanged(qint64 thread_id, int peer_uid)
{
    const bool group = _kind == static_cast<int>(ChatThreadKind::Group);
    const bool match = group ? thread_id == _threadId
                             : (thread_id == _threadId
                                || (peer_uid != 0 && peer_uid == _peerUid));
    if (!match)
        return;

    auto thread = threadData();
    if (!thread) {
        if (_cachedCount > 0) {
            beginResetModel();
            _cachedCount = 0;
            _cachedIds.clear();
            _cachedStatus.clear();
            endResetModel();
            emit countChanged();
        }
        return;
    }

    const auto &msgs = thread->_chat_msgs;
    const int newCount = static_cast<int>(msgs.size());

    if (newCount == _cachedCount) {
        bool sameOrder = true;
        for (int i = 0; i < newCount; ++i) {
            if (!msgs[i] || msgs[i]->_msg_id != _cachedIds[i]) {
                sameOrder = false;
                break;
            }
        }
        if (!sameOrder) {
            beginResetModel();
            syncCache(msgs);
            endResetModel();
            emit countChanged();
            return;
        }
        // 行数与顺序都没变，只可能是状态/内容更新；按行刷新，不重建列表
        if (newCount > 0) {
            syncCache(msgs);
            emit dataChanged(index(0, 0), index(newCount - 1, 0));
        }
        return;
    }

    if (newCount > _cachedCount) {
        bool prefix = true;
        for (int i = 0; i < _cachedCount; ++i) {
            if (!msgs[i] || msgs[i]->_msg_id != _cachedIds[i]) {
                prefix = false;
                break;
            }
        }
        if (prefix) {
            //插入前先刷新旧行：原「最后一条自己发的」不再显示已读/未读
            if (_cachedCount > 0)
                emit dataChanged(index(0, 0), index(_cachedCount - 1, 0), {StatusTextRole});
            beginInsertRows(QModelIndex(), _cachedCount, newCount - 1);
            syncCache(msgs);
            endInsertRows();
            emit countChanged();
            return;
        }
    }

    // 数量减少，或待确认消息拿到 message_id 后改变了位置
    beginResetModel();
    syncCache(msgs);
    endResetModel();
    emit countChanged();
}
