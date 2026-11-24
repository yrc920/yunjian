#include "grouplistmodel.h"

#include "usermgr.h"
#include "userdata.h"

GroupListModel::GroupListModel(QObject *parent) : QAbstractListModel(parent)
{
    auto *mgr = UserMgr::GetInstance().get();
    connect(mgr, &UserMgr::GroupListChanged, this, &GroupListModel::reload);
    // 群名、成员数、角色、免打扰都可能变；群列表很小，整表刷新比逐字段判断更稳
    connect(mgr, &UserMgr::GroupInfoChanged, this,
            [this](qint64) { reload(); });
    connect(mgr, &UserMgr::GroupIconChanged, this, [this](qint64 threadId) {
        const int row = indexOfThread(threadId);
        if (row < 0)
            return;
        const QModelIndex idx = index(row, 0);
        emit dataChanged(idx, idx, { IconRole });
    });
    reload();
}

int GroupListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : _view.size();
}

std::shared_ptr<GroupInfo> GroupListModel::groupAtRow(int row) const
{
    if (row < 0 || row >= _view.size())
        return nullptr;
    return UserMgr::GetInstance()->GroupAt(_view[row]);
}

QVariant GroupListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= _view.size())
        return {};
    auto group = groupAtRow(index.row());
    if (!group)
        return {};

    switch (role) {
    case ThreadIdRole:
        return QVariant::fromValue(group->_thread_id);
    case NameRole:
        return group->_name;
    case IconRole:
        return UserMgr::GetInstance()->GroupIconPath(group->_thread_id);
    case MemberCountRole:
        return group->_member_count;
    case RoleRole:
        return group->_my_role;
    case MutedRole:
        return group->_muted;
    case LeftStateRole:
        return group->_left_state;
    case InitialRole:
        return group->_name.left(1);
    default:
        return {};
    }
}

QHash<int, QByteArray> GroupListModel::roleNames() const
{
    return {
        { ThreadIdRole, "threadId" },
        { NameRole, "name" },
        { IconRole, "icon" },
        { MemberCountRole, "memberCount" },
        { RoleRole, "role" },
        { MutedRole, "muted" },
        { LeftStateRole, "leftState" },
        { InitialRole, "initial" }
    };
}

void GroupListModel::setFilter(const QString &text)
{
    if (_filter == text)
        return;
    _filter = text;
    reload();
}

int GroupListModel::indexOfThread(qint64 threadId) const
{
    for (int row = 0; row < _view.size(); ++row) {
        auto group = groupAtRow(row);
        if (group && group->_thread_id == threadId)
            return row;
    }
    return -1;
}

QVariantMap GroupListModel::get(int row) const
{
    QVariantMap map;
    if (row < 0 || row >= _view.size())
        return map;
    const QModelIndex idx = index(row, 0);
    // roleNames() 返回临时 QHash，迭代器必须来自同一份实例，不能每次调用重取
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        map.insert(QString::fromUtf8(it.value()), data(idx, it.key()));
    return map;
}

void GroupListModel::reload()
{
    beginResetModel();
    _view.clear();
    _total = 0;
    auto *mgr = UserMgr::GetInstance().get();
    const int size = mgr->GroupCount();
    for (int i = 0; i < size; ++i) {
        auto group = mgr->GroupAt(i);
        if (!group || group->_left_state != 0)
            continue;
        ++_total;
        if (!_filter.isEmpty()
            && !group->_name.contains(_filter, Qt::CaseInsensitive))
            continue;
        _view.push_back(i);
    }
    endResetModel();
    emit countChanged();
}
