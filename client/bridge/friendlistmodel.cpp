#include "friendlistmodel.h"

#include "usermgr.h"
#include "userdata.h"

FriendListModel::FriendListModel(QObject *parent) : QAbstractListModel(parent)
{
    connect(UserMgr::GetInstance().get(), &UserMgr::FriendListAboutToChange,
            this, [this] { beginResetModel(); });
    auto *mgr = UserMgr::GetInstance().get();
    connect(mgr, &UserMgr::FriendListChanged, this, &FriendListModel::onFriendListChanged);
    connect(mgr, &UserMgr::FriendChatChanged, this, [this](int uid) {
        const int row = indexOfUid(uid);
        if (row < 0)
            return;
        // 只刷新这一行的「最后一条消息」，不整表重置，避免列表闪烁
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {LastTextRole});
    });
    // 头像下载完成只影响这一行，避免整表重置
    connect(mgr, &UserMgr::FriendIconChanged, this, [this](int uid) {
        const int row = indexOfUid(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {IconRole});
    });
    // 备注或标签变化会改显示名与分组；筛选状态下这条记录可能进出结果集，整表重置
    connect(mgr, &UserMgr::FriendRelationChanged, this, [this](int uid) {
        if (_filterEnabled && !_tagFilter.isEmpty()) {
            beginResetModel();
            endResetModel();
            emit countChanged();
            return;
        }
        const int row = indexOfUid(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {DisplayNameRole, RemarkRole, TagsRole, InitialRole});
    });
    // 好友改了昵称或签名：显示名与资料页摘要跟着变，备注与标签不受影响
    connect(mgr, &UserMgr::FriendProfileChanged, this, [this](int uid) {
        const int row = indexOfUid(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {NickRole, DisplayNameRole, DescRole, InitialRole});
    });
}

// 行数按当前筛选视图计算；标签为空或筛选关闭时就是好友总数
int FriendListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    auto *mgr = UserMgr::GetInstance().get();
    if (!_filterEnabled || _tagFilter.isEmpty())
        return mgr->FriendCount();

    int count = 0;
    const int total = mgr->FriendCount();
    for (int row = 0; row < total; ++row) {
        auto info = mgr->FriendAt(row);
        if (info && info->_tags.contains(_tagFilter))
            ++count;
    }
    return count;
}

// 不受筛选影响的好友总数，空态与入口显隐都按它判断
int FriendListModel::totalCount() const
{
    return UserMgr::GetInstance()->FriendCount();
}

// 取第 row 行对应的好友：未筛选时直接按行号，筛选时跳过不匹配的条目
std::shared_ptr<FriendInfo> FriendListModel::friendAtRow(int row) const
{
    auto *mgr = UserMgr::GetInstance().get();
    const int total = mgr->FriendCount();
    if (!_filterEnabled || _tagFilter.isEmpty()) {
        if (row < 0 || row >= total)
            return nullptr;
        return mgr->FriendAt(row);
    }

    int seen = 0;
    for (int index = 0; index < total; ++index) {
        auto info = mgr->FriendAt(index);
        if (!info || !info->_tags.contains(_tagFilter))
            continue;
        if (seen == row)
            return info;
        ++seen;
    }
    return nullptr;
}

// 切换标签筛选：行视图整体变化，直接用 reset 而不是逐行增删
void FriendListModel::setTagFilter(const QString &tag)
{
    if (_tagFilter == tag)
        return;
    beginResetModel();
    _tagFilter = tag;
    endResetModel();
    emit countChanged();
}

// 聊天模式关闭筛选，避免会话列表被联系人页选中的标签一起过滤
void FriendListModel::setFilterEnabled(bool enabled)
{
    if (_filterEnabled == enabled)
        return;
    beginResetModel();
    _filterEnabled = enabled;
    endResetModel();
    emit countChanged();
}

// 按角色取字段；lastText 为空时界面显示「还没有消息」
QVariant FriendListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= rowCount())
        return {};
    auto friend_info = friendAtRow(index.row());
    if (!friend_info)
        return {};

    switch (role) {
    case UidRole: return friend_info->_uid;
    case NameRole: return friend_info->_name;
    case NickRole: return friend_info->_nick;
    case DisplayNameRole:
        return DisplayName(friend_info->_back, friend_info->_nick, friend_info->_name);
    case RemarkRole: return friend_info->_back;
    case TagsRole: return friend_info->_tags;
    case IconRole: return friend_info->_icon;
    case DescRole: return friend_info->_desc;
    case LastTextRole:
        if (!friend_info->_chat_thread->_chat_msgs.empty()) {
            const auto &last = friend_info->_chat_thread->_chat_msgs.back();
            return last->_imageSource.isEmpty() ? last->_msg_content : QStringLiteral("[图片]");
        }
        return friend_info->_last_msg;
    case ThreadIdRole:
        return friend_info->_chat_thread->_thread_id;
    case InitialRole: {
        //首字取显示名，备注改了分组字母也跟着改
        const QString name = DisplayName(friend_info->_back, friend_info->_nick,
                                         friend_info->_name);
        return name.isEmpty() ? QStringLiteral("#") : name.left(1).toUpper();
    }
    default:
        return {};
    }
}

// 暴露给 QML 的角色名，delegate 用 model.xxx 读取
QHash<int, QByteArray> FriendListModel::roleNames() const
{
    return {
        {UidRole, "uid"},
        {NameRole, "name"},
        {NickRole, "nick"},
        {DisplayNameRole, "displayName"},
        {RemarkRole, "remark"},
        {TagsRole, "tags"},
        {IconRole, "icon"},
        {DescRole, "desc"},
        {LastTextRole, "lastText"},
        {InitialRole, "initial"},
        {ThreadIdRole, "threadId"},
    };
}

// 按 uid 找当前视图中的行号；筛选时不在结果里的好友返回 -1，调用方跳过刷新
int FriendListModel::indexOfUid(int uid) const
{
    const int rows = rowCount();
    for (int row = 0; row < rows; ++row) {
        auto friend_info = friendAtRow(row);
        if (friend_info && friend_info->_uid == uid)
            return row;
    }
    return -1;
}

// 聊天模式只展示已经有 thread 的会话；空态判断不能用好友总数
int FriendListModel::chatCount() const
{
    int count = 0;
    const int total = UserMgr::GetInstance()->FriendCount();
    for (int row = 0; row < total; ++row) {
        auto friend_info = UserMgr::GetInstance()->FriendAt(row);
        if (friend_info && friend_info->_chat_thread->_thread_id > 0)
            ++count;
    }
    return count;
}

//把一行摊平成 map，字段与 roleNames 保持一致，方便 QML 做搜索结果等一次性读取
QVariantMap FriendListModel::get(int row) const
{
    const QModelIndex idx = index(row, 0);
    return {
        {QStringLiteral("uid"), data(idx, UidRole)},
        {QStringLiteral("name"), data(idx, NameRole)},
        {QStringLiteral("nick"), data(idx, NickRole)},
        {QStringLiteral("displayName"), data(idx, DisplayNameRole)},
        {QStringLiteral("remark"), data(idx, RemarkRole)},
        {QStringLiteral("tags"), data(idx, TagsRole)},
        {QStringLiteral("icon"), data(idx, IconRole)},
        {QStringLiteral("desc"), data(idx, DescRole)},
        {QStringLiteral("lastText"), data(idx, LastTextRole)},
        {QStringLiteral("initial"), data(idx, InitialRole)},
        {QStringLiteral("threadId"), data(idx, ThreadIdRole)},
    };
}

// 好友列表整体变化（登录、通过验证）：整表重置
void FriendListModel::onFriendListChanged()
{
    endResetModel();
    emit countChanged();
}
