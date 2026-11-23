#ifndef GROUPLISTMODEL_H
#define GROUPLISTMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>

struct GroupInfo;

/**
 * @brief 联系人页的群聊列表
 *
 * 只读模型：从 UserMgr 的群列表取数据，不复制业务字段。
 * 只显示仍在群里的会话（被移出 / 已解散的不进联系人），按加入顺序排列。
 * 搜索只在本地按群名过滤，不触发服务端请求。
 */
class GroupListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    //不受搜索影响的群总数，用于空态与标题计数
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)

public:
    enum Roles {
        ThreadIdRole = Qt::UserRole + 1,
        NameRole,
        IconRole,
        MemberCountRole,
        RoleRole,     // 0 成员 / 1 管理员 / 2 群主
        MutedRole,
        LeftStateRole,
        InitialRole
    };

    explicit GroupListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int totalCount() const { return _total; }

    Q_INVOKABLE void setFilter(const QString &text);
    Q_INVOKABLE int indexOfThread(qint64 threadId) const;
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    void reload();
    std::shared_ptr<GroupInfo> groupAtRow(int row) const;

    QVector<int> _view; // _group_list 的下标，按当前筛选
    QString _filter;
    int _total = 0;
};

#endif // GROUPLISTMODEL_H
