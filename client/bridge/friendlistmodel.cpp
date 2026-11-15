#include "friendlistmodel.h"

#include "usermgr.h"
#include "userdata.h"

FriendListModel::FriendListModel(QObject *parent) : QAbstractListModel(parent)
{
    auto *mgr = UserMgr::GetInstance().get();
    connect(mgr, &UserMgr::FriendListAboutToChange, this, [this] { beginResetModel(); });
    connect(mgr, &UserMgr::FriendListChanged, this, &FriendListModel::onFriendListChanged);
    // 备注或标签变化会改显示名与分组；筛选状态下这条记录可能进出结果集，整表重置
    connect(mgr, &UserMgr::FriendRelationChanged, this, [this](int uid) {
        if (!_tagFilter.isEmpty()) {
            beginResetModel();
            _viewUids = computeView();
            endResetModel();
            emit countChanged();
            return;
        }
        const int row = _viewUids.indexOf(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx,
            {DisplayNameRole, RemarkRole, TagsRole, InitialRole});
    });
    //好友改了昵称：显示名与资料页摘要跟着变，备注与标签不受影响
    connect(mgr, &UserMgr::FriendProfileChanged, this, [this](int uid) {
        const int row = _viewUids.indexOf(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {NickRole, DisplayNameRole, DescRole, InitialRole});
    });
    // 头像下载完成只影响这一行
    connect(mgr, &UserMgr::FriendIconChanged, this, [this](int uid) {
        const int row = _viewUids.indexOf(uid);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, {IconRole});
    });
    //标签集合变化后筛选条会重算，当前选中的标签可能已经不存在
    connect(mgr, &UserMgr::FriendTagsChanged, this, [this] {
        if (_tagFilter.isEmpty())
            return;
        beginResetModel();
        _viewUids = computeView();
        endResetModel();
        emit countChanged();
    });
}

//按标签筛选取行视图；空串表示全部联系人，顺序保持加好友顺序
QVector<int> FriendListModel::computeView() const
{
    auto *mgr = UserMgr::GetInstance().get();
    QVector<int> view;
    const int total = mgr->FriendCount();
    view.reserve(total);
    for (int i = 0; i < total; ++i) {
        auto info = mgr->FriendAt(i);
        if (!info)
            continue;
        if (!_tagFilter.isEmpty() && !info->_tags.contains(_tagFilter))
            continue;
        view.push_back(info->_uid);
    }
    return view;
}

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

// 按角色取字段；displayName 统一走备注 > 昵称 > 用户名
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
        {InitialRole, "initial"},
    };
}

// 按 uid 找当前视图中的行号；筛选时不在结果里的好友返回 -1，调用方跳过刷新
int FriendListModel::indexOfUid(int uid) const
{
    return _viewUids.indexOf(uid);
}

//把一行摊平成 map，字段与 roleNames 保持一致
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
        {QStringLiteral("initial"), data(idx, InitialRole)},
    };
}

// 好友列表整体变化（登录、通过验证）：按当前筛选重建行视图
void FriendListModel::onFriendListChanged()
{
    _viewUids = computeView();
    endResetModel();
    emit countChanged();
}
