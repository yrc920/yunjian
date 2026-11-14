#include "messagemodel.h"

#include "usermgr.h"
#include "userdata.h"
#include "timeformat.h"

MessageModel::MessageModel(QObject *parent) : QAbstractListModel(parent)
{
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendListAboutToChange,
            this, [this] { beginResetModel(); });
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendListChanged, this, [this] {
        auto info = UserMgr::GetInstance()->GetFriendById(_uid);
        if (info)
            syncCache(info->_chat_thread->_chat_msgs);
        else {
            _cachedCount = 0;
            _cachedIds.clear();
            _cachedStatus.clear();
        }
        endResetModel();
        emit countChanged();
    });
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendChatChanged,
            this, &MessageModel::onFriendChatChanged);
    connect(UserMgr::GetInstance().get(), &UserMgr::UserIconChanged, this, [this] {
        auto *mgr = UserMgr::GetInstance().get();
        const auto info = mgr->GetFriendById(_uid);
        if (!info)
            return;
        const int count = qMin(_cachedCount, static_cast<int>(info->_chat_thread->_chat_msgs.size()));
        for (int row = 0; row < count; ++row) {
            if (info->_chat_thread->_chat_msgs.at(row)->_from_uid == mgr->GetUid())
                emit dataChanged(index(row, 0), index(row, 0), {AvatarRole});
        }
    });
    // 好友头像换成本地缓存后，刷新对方消息行的头像
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendIconChanged, this,
            [this](int uid) {
        auto *mgr = UserMgr::GetInstance().get();
        const auto info = mgr->GetFriendById(_uid);
        if (!info)
            return;
        const int count = qMin(_cachedCount, static_cast<int>(info->_chat_thread->_chat_msgs.size()));
        for (int row = 0; row < count; ++row) {
            if (info->_chat_thread->_chat_msgs.at(row)->_from_uid == uid)
                emit dataChanged(index(row, 0), index(row, 0), {AvatarRole});
        }
    });
    // 备注、昵称或签名变化后，气泡上的名字按新的显示名重绘；头像与内容不变
    auto refreshNames = [this] {
        const int rows = rowCount();
        for (int row = 0; row < rows; ++row)
            emit dataChanged(index(row, 0), index(row, 0), {NameRole});
    };
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendRelationChanged, this,
            [this, refreshNames](int uid) {
        if (uid == _uid)
            refreshNames();
    });
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendProfileChanged, this,
            [this, refreshNames](int uid) {
        if (uid == _uid)
            refreshNames();
    });
    connect(UserMgr::GetInstance().get(), &UserMgr::UserProfileChanged, this, refreshNames);
}

// 切换当前会话；uid 为 0 时清空列表
void MessageModel::setFriend(int uid)
{
    beginResetModel();
    _uid = uid;
    auto friend_info = UserMgr::GetInstance()->GetFriendById(uid);
    if (friend_info)
        syncCache(friend_info->_chat_thread->_chat_msgs);
    else {
        _cachedCount = 0;
        _cachedIds.clear();
        _cachedStatus.clear();
    }
    endResetModel();
    emit countChanged();
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

// 行数取当前好友的消息条数
int MessageModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() || _uid == 0)
        return 0;
    return _cachedCount;
}

// 按角色取字段；isSelf 决定气泡左右
QVariant MessageModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= rowCount())
        return {};
    auto friend_info = UserMgr::GetInstance()->GetFriendById(_uid);
    if (!friend_info || index.row() < 0
        || index.row() >= static_cast<int>(friend_info->_chat_thread->_chat_msgs.size())) {
        return {};
    }

    const auto &msg = friend_info->_chat_thread->_chat_msgs.at(index.row());
    switch (role) {
    case TextRole: return msg->_msg_content;
    case TimeTextRole: {
        if (msg->_time <= 0)
            return {};
        //与上一条间隔不到 5 分钟就沿用上一条的时间条，避免每句都挂一个时间
        if (index.row() > 0) {
            const auto &prev = friend_info->_chat_thread->_chat_msgs.at(index.row() - 1);
            if (prev && prev->_time > 0
                && msg->_time - prev->_time < TimeFormat::kSeparatorSeconds)
                return {};
        }
        return TimeFormat::messageTime(msg->_time);
    }
    case SelfRole: return msg->_from_uid == UserMgr::GetInstance()->GetUid();
    case FromUidRole: return msg->_from_uid;
    case ImageSourceRole: return msg->_imageSource;
    case ImageWidthRole: return msg->_imageWidth;
    case ImageHeightRole: return msg->_imageHeight;
    case AvatarRole:
    case NameRole: {
        auto self = UserMgr::GetInstance()->GetUserInfo();
        if (self && msg->_from_uid == self->_uid) {
            //自己的气泡用昵称优先；昵称为空时回落到用户名
            return role == AvatarRole ? UserMgr::GetInstance()->GetIcon()
                                      : DisplayName(QString(), self->_nick, self->_name);
        }
        //对方气泡与好友列表保持同一条规则：备注 > 昵称 > 用户名
        return role == AvatarRole ? friend_info->_icon
                                  : DisplayName(friend_info->_back, friend_info->_nick,
                                                friend_info->_name);
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
    };
}

// 当前会话消息变化：顺序不变时按增量插入或按行刷新，只有重排/缩表才 reset
void MessageModel::onFriendChatChanged(int uid)
{
    if (uid != _uid || _uid == 0)
        return;

    auto friend_info = UserMgr::GetInstance()->GetFriendById(uid);
    if (!friend_info) {
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

    const auto &msgs = friend_info->_chat_thread->_chat_msgs;
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
