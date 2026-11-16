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
 * 联系人列表专用：按标签筛选，行顺序保持加好友顺序。
 * 会话列表由 ConversationModel 承担（单聊与群聊合成一张表），
 * 两个模型都直接引用 UserMgr 里的对象，不复制业务数据。
 */
class FriendListModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged) //当前视图行数（受筛选影响）
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged) //好友总数，用于空态判断

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
        InitialRole,   // 显示名首字符，供联系人列表分组
    };

    explicit FriendListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE int indexOfUid(int uid) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    int totalCount() const; //不受筛选影响的好友总数

    // 按标签筛选；空串表示不筛选。筛选变化会重置模型，行号随之失效
    Q_INVOKABLE void setTagFilter(const QString &tag);

signals:
    void countChanged();

private:
    //按当前标签筛选取出行视图，不改动 UserMgr 的原始顺序
    QVector<int> computeView() const;
    //按当前行视图取第 row 行对应的好友，行号越界返回空
    std::shared_ptr<FriendInfo> friendAtRow(int row) const;

    QVector<int> _viewUids;
    QString _tagFilter;
};

#endif // FRIENDLISTMODEL_H
