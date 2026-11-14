#include "applylistmodel.h"

#include "usermgr.h"
#include "userdata.h"

ApplyListModel::ApplyListModel(QObject *parent) : QAbstractListModel(parent)
{
    connect(UserMgr::GetInstance().get(), &UserMgr::ApplyListAboutToChange,
            this, [this] { beginResetModel(); });
    connect(UserMgr::GetInstance().get(), &UserMgr::ApplyListChanged,
            this, &ApplyListModel::onApplyListChanged);
    // 头像下载完成只换这一行的 icon，不整表重置，申请列表不会闪
    connect(UserMgr::GetInstance().get(), &UserMgr::ApplyIconChanged,
            this, [this](int uid) {
                const int total = UserMgr::GetInstance()->ApplyCount(); //当前申请条数
                for (int row = 0; row < total; ++row) { //同一个uid可能同时出现在收/发两行
                    auto info = UserMgr::GetInstance()->ApplyAt(row);
                    if (!info || info->_uid != uid)
                        continue; //不是这次更新的申请人，跳过
                    const QModelIndex idx = index(row, 0);
                    emit dataChanged(idx, idx, {IconRole}); //只刷icon角色，QML会重新取图
                }
            });
}

// 行数取 UserMgr 的申请条数
int ApplyListModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return UserMgr::GetInstance()->ApplyCount();
}

// 按角色取字段；status 为 0 表示还显示「添加」按钮
QVariant ApplyListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.model() != this || index.column() != 0
        || index.row() < 0 || index.row() >= rowCount())
        return {};
    auto info = UserMgr::GetInstance()->ApplyAt(index.row());
    if (!info)
        return {};

    switch (role) {
    case UidRole: return info->_uid;
    case NameRole: return info->_name;
    case NickRole: return info->_nick;
    case DisplayNameRole: return DisplayName(QString(), info->_nick, info->_name);
    case IconRole: return info->_icon;
    case DescRole: return info->_desc;
    case StatusRole: return info->_status;
    case DirectionRole: return info->_direction;
    default: return {};
    }
}

// 暴露给 QML 的角色名
QHash<int, QByteArray> ApplyListModel::roleNames() const
{
    return {
        {UidRole, "uid"},
        {NameRole, "name"},
        {NickRole, "nick"},
        {DisplayNameRole, "displayName"},
        {IconRole, "icon"},
        {DescRole, "desc"},
        {StatusRole, "status"},
        {DirectionRole, "direction"},
    };
}

// 只有别人申请我的未处理记录算待处理，我发出去的不占红点
int ApplyListModel::pendingCount() const
{
    int pending = 0;
    const int total = UserMgr::GetInstance()->ApplyCount();
    for (int row = 0; row < total; ++row) {
        auto info = UserMgr::GetInstance()->ApplyAt(row);
        if (info && info->_status == 0 && info->_direction == 0)
            ++pending;
    }
    return pending;
}

//把一行摊平成 map，字段与 roleNames 保持一致
QVariantMap ApplyListModel::get(int row) const
{
    const QModelIndex idx = index(row, 0);
    return {
        {QStringLiteral("uid"), data(idx, UidRole)},
        {QStringLiteral("name"), data(idx, NameRole)},
        {QStringLiteral("nick"), data(idx, NickRole)},
        {QStringLiteral("displayName"), data(idx, DisplayNameRole)},
        {QStringLiteral("icon"), data(idx, IconRole)},
        {QStringLiteral("desc"), data(idx, DescRole)},
        {QStringLiteral("status"), data(idx, StatusRole)},
        {QStringLiteral("direction"), data(idx, DirectionRole)},
    };
}

// 申请列表变化（新增、状态变更）：整表重置
void ApplyListModel::onApplyListChanged()
{
    endResetModel();
    emit countChanged();
}
