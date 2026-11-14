#ifndef FRIENDLISTMODEL_H
#define FRIENDLISTMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QVector>
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
 * 两个视图差异由两个开关表达：
 *   - 标签筛选：只作用于联系人模式，按标签过滤行；
 *   - 最近排序：只作用于会话模式，按最后一条消息时间倒序。
 * 顺序与过滤都落在 _viewUids 这一份行视图上，UserMgr 里的原始顺序不动。
 */
class FriendListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged) //当前视图行数（受筛选影响）
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
        LastTimeRole,  // 最后一条消息的时间，按"今天/昨天/周几/日期"分层显示
        UnreadRole,    // 该会话的未读条数，0 表示不显示徽标
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
    // 会话模式按最后消息时间倒序；联系人模式保持加好友顺序
    Q_INVOKABLE void setSortByRecent(bool enabled);

signals:
    void countChanged();

private:
    void onFriendListChanged();
    //按当前筛选与排序算出完整的行视图，不改动已有行
    QVector<int> computeView() const;
    //最后一条有时间的消息；没有会话或全是本地占位消息时返回 0
    static qint64 lastMessageTime(const std::shared_ptr<FriendInfo> &info);
    //按当前行视图取第 row 行对应的好友，行号越界返回空
    std::shared_ptr<FriendInfo> friendAtRow(int row) const;

    QVector<int> _viewUids;
    QString _tagFilter;
    bool _filterEnabled = false;
    bool _sortRecent = false;
};

#endif // FRIENDLISTMODEL_H
