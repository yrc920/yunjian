#include "friendlistmodel.h"

#include <algorithm>

#include "usermgr.h"
#include "userdata.h"
#include "timeformat.h"

FriendListModel::FriendListModel(QObject *parent) : QAbstractListModel(parent)
{
    auto *mgr = UserMgr::GetInstance().get();
    connect(mgr, &UserMgr::FriendListAboutToChange, this, [this] { beginResetModel(); });
    connect(mgr, &UserMgr::FriendListChanged, this, &FriendListModel::onFriendListChanged);
    //消息变化：会话模式可能改变排序，先算新顺序，顺序没变才只刷新这一行
    connect(mgr, &UserMgr::FriendChatChanged, this, [this](int uid) {
        if (_sortRecent) {
            QVector<int> next = computeView();
            if (next != _viewUids) {
                beginResetModel();
                _viewUids = std::move(next);
                endResetModel();
                emit countChanged();
                return;
            }
        }
        const int row = indexOfUid(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {LastTextRole, LastTimeRole});
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
            _viewUids = computeView();
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
    // 未读数变化：整列刷新 UnreadRole 就够了，行数很少，不做逐行定位
    connect(mgr, &UserMgr::UnreadChanged, this, [this] {
        const int rows = rowCount();
        if (rows <= 0)
            return;
        emit dataChanged(index(0, 0), index(rows - 1, 0), {UnreadRole});
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

//最后一条有时间的消息。按 message_id 升序排列，从尾部往前找第一个带时间的，
//跳过刚发出、服务端还没回执的本地占位消息
qint64 FriendListModel::lastMessageTime(const std::shared_ptr<FriendInfo> &info)
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
    //历史还没拉回来时用登录下发的线程时间，避免首屏顺序先按加好友顺序再跳
    return info->_last_time;
}

//按筛选与排序算出完整的行视图。会话模式按最后消息时间倒序，没有消息的会话排最后，
//相同时间保持加好友顺序（stable_sort），避免列表无谓跳动
QVector<int> FriendListModel::computeView() const
{
    auto *mgr = UserMgr::GetInstance().get();
    const int total = mgr->FriendCount();

    std::vector<std::shared_ptr<FriendInfo>> items;
    items.reserve(total);
    for (int i = 0; i < total; ++i) {
        auto info = mgr->FriendAt(i);
        if (!info)
            continue;
        if (_filterEnabled && !_tagFilter.isEmpty() && !info->_tags.contains(_tagFilter))
            continue;
        items.push_back(std::move(info));
    }

    if (_sortRecent) {
        std::stable_sort(items.begin(), items.end(),
            [](const std::shared_ptr<FriendInfo> &left, const std::shared_ptr<FriendInfo> &right) {
                return lastMessageTime(left) > lastMessageTime(right);
            });
    }

    QVector<int> view;
    view.reserve(static_cast<int>(items.size()));
    for (const auto &info : items)
        view.push_back(info->_uid);
    return view;
}

// 行数取当前行视图的长度；筛选与排序都只影响它，不动 UserMgr
int FriendListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return _viewUids.size();
}

// 不受筛选影响的好友总数，空态与入口显隐都按它判断
int FriendListModel::totalCount() const
{
    return UserMgr::GetInstance()->FriendCount();
}

// 取第 row 行对应的好友：行视图里存的是 uid，数据始终回 UserMgr 取
std::shared_ptr<FriendInfo> FriendListModel::friendAtRow(int row) const
{
    if (row < 0 || row >= _viewUids.size())
        return nullptr;
    return UserMgr::GetInstance()->GetFriendById(_viewUids[row]);
}

// 切换标签筛选：行视图整体变化，直接用 reset 而不是逐行增删
void FriendListModel::setTagFilter(const QString &tag)
{
    if (_tagFilter == tag)
        return;
    beginResetModel();
    _tagFilter = tag;
    _viewUids = computeView();
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
    _viewUids = computeView();
    endResetModel();
    emit countChanged();
}

// 会话模式按最近消息排序；联系人模式回到加好友顺序
void FriendListModel::setSortByRecent(bool enabled)
{
    if (_sortRecent == enabled)
        return;
    beginResetModel();
    _sortRecent = enabled;
    _viewUids = computeView();
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
    case LastTimeRole:
        return TimeFormat::listTime(lastMessageTime(friend_info));
    case UnreadRole:
        return UserMgr::GetInstance()->UnreadCount(friend_info->_uid);
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
        {LastTimeRole, "lastTime"},
        {UnreadRole, "unread"},
        {InitialRole, "initial"},
        {ThreadIdRole, "threadId"},
    };
}

// 按 uid 找当前视图中的行号；筛选时不在结果里的好友返回 -1，调用方跳过刷新
int FriendListModel::indexOfUid(int uid) const
{
    return _viewUids.indexOf(uid);
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
        {QStringLiteral("lastTime"), data(idx, LastTimeRole)},
        {QStringLiteral("unread"), data(idx, UnreadRole)},
        {QStringLiteral("initial"), data(idx, InitialRole)},
        {QStringLiteral("threadId"), data(idx, ThreadIdRole)},
    };
}

// 好友列表整体变化（登录、通过验证）：按当前筛选与排序重建行视图
void FriendListModel::onFriendListChanged()
{
    _viewUids = computeView();
    endResetModel();
    emit countChanged();
}
