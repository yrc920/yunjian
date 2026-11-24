#include "conversationfiltermodel.h"

#include "conversationmodel.h"

ConversationFilterModel::ConversationFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
    // 过滤后行数变化要通知 QML 的计数与“没有找到”提示
    connect(this, &QAbstractItemModel::rowsInserted, this, &ConversationFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &ConversationFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &ConversationFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &ConversationFilterModel::countChanged);
}

void ConversationFilterModel::setSource(ConversationModel *source)
{
    setSourceModel(source);
    const auto roles = source->roleNames();
    _nameRole = roles.key("displayName");
    _textRole = roles.key("lastText");
    emit countChanged();
}

int ConversationFilterModel::count() const
{
    return rowCount();
}

QString ConversationFilterModel::filterText() const
{
    return _filter;
}

void ConversationFilterModel::setFilterText(const QString &text)
{
    if (_filter == text)
        return;
    _filter = text;
    invalidateFilter();
    emit filterTextChanged();
}

bool ConversationFilterModel::filterAcceptsRow(int source_row, const QModelIndex &source_parent) const
{
    const QString keyword = _filter.trimmed();
    if (keyword.isEmpty() || !sourceModel())
        return true;
    const QModelIndex idx = sourceModel()->index(source_row, 0, source_parent);
    const QString name = idx.data(_nameRole).toString();
    const QString text = idx.data(_textRole).toString();
    return name.contains(keyword, Qt::CaseInsensitive)
        || text.contains(keyword, Qt::CaseInsensitive);
}
