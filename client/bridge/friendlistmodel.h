#ifndef FRIENDLISTMODEL_H
#define FRIENDLISTMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QtQml/qqmlregistration.h>
#include <memory>

struct FriendInfo;

/**
 * @brief 好友列表模型
 *
 * 会话列表与联系人列表读的是同一份好友数据，只是 delegate 显示的东西不同，
 * 因此共用一个模型，不复制两份数据。行数据直接引用 UserMgr 里的对象，
 * UserMgr 发出变更信号时模型跟着刷新。
 *
 * 联系人模式支持按标签筛选：筛选只改变模型的行视图，不动 UserMgr 里的原始数据。
 * 聊天模式会关闭筛选，否则会话列表也会跟着被过滤。
 */
class FriendListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged) //当前视图行数（受标签筛选影响）
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged) //好友总数，用于空态判断
    Q_PROPERTY(int chatCount READ chatCount NOTIFY countChanged)

public:
    enum Roles {
        UidRole = Qt::UserRole + 1,
        NameRole,     // 用户名，用于资料页与搜索结果确认身份
        NickRole,     // 用户自己设的昵称
        DisplayNameRole, // 备注 > 昵称 > 用户名，列表与标题统一用它
        RemarkRole,   // 自己给好友设的备注，空表示没设
        TagsRole,     // 自己给好友打的分组标签
        IconRole,
        DescRole,
        LastTextRole,
        InitialRole,   // 显示名首字符，供联系人列表分组
        ThreadIdRole,
    };

    explicit FriendListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int indexOfUid(int uid) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    int chatCount() const; //会话题（thread_id > 0）数量，供聊天模式空态使用
    int totalCount() const; //不受筛选影响的好友总数

    // 按标签筛选；空串表示不筛选。筛选变化会重置模型，行号随之失效
    Q_INVOKABLE void setTagFilter(const QString &tag);
    // 聊天模式关闭筛选，联系人模式开启；切换时同样重置模型
    Q_INVOKABLE void setFilterEnabled(bool enabled);

signals:
    void countChanged();

private:
    void onFriendListChanged();
    //按当前筛选视图取第 row 行对应的好友，行号越界返回空
    std::shared_ptr<FriendInfo> friendAtRow(int row) const;

    QString _tagFilter;
    bool _filterEnabled = false;
};

#endif // FRIENDLISTMODEL_H
