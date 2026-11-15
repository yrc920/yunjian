#ifndef CONVERSATIONMODEL_H
#define CONVERSATIONMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

struct FriendInfo;
struct GroupInfo;

/**
 * @brief 会话列表模型：把单聊与群聊合成一张按最近消息排序的表
 *
 * 行只保存 {类型, uid, thread_id}，字段实时从 UserMgr 读取，不复制业务数据。
 * 联系人列表仍用 FriendListModel；两者分开后，会话行不再假设"一行必须是好友"，
 * 群聊、已删好友的遗留会话和系统消息都能在同一张表里表达。
 */
class ConversationModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    //已经有会话记录的条数：空态判断用它，不能用好友总数
    Q_PROPERTY(int activeCount READ activeCount NOTIFY countChanged)

public:
    enum Roles {
        KindRole = Qt::UserRole + 1, //1 单聊 / 2 群聊
        ThreadIdRole,
        UidRole, //单聊对方 uid，群聊为 0
        DisplayNameRole,
        AvatarRole, //群聊为空，界面按名字首字兜底
        InitialRole,
        LastTextRole,
        LastTimeRole,
        UnreadRole,
        MutedRole, //群免打扰：行内不显示红点数字
        AtMeRole, //群未读里有人 @我
        MemberCountRole,
        SubtitleRole, //群聊显示"3 人"，单聊为空
    };

    explicit ConversationModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int activeCount() const;

    //按 thread 找行号，找不到返回 -1
    Q_INVOKABLE int indexOfThread(qint64 thread_id) const;
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();

private:
    struct Row {
        int kind = 1;
        int uid = 0;
        qint64 thread = 0;
    };

    //按最后一条消息时间倒序；没有消息的会话排最后，相同时间保持原顺序
    void rebuild();
    void refreshRow(int row);
    QVector<Row> computeRows() const;
    qint64 lastMessageTime(const Row &row) const;
    qint64 lastMessageTime(const std::shared_ptr<FriendInfo> &info) const;
    qint64 lastMessageTime(const std::shared_ptr<GroupInfo> &info) const;
    int indexOfRow(const Row &row) const;
    int indexOfThreadInternal(qint64 thread_id) const;
    QString lastText(const std::shared_ptr<FriendInfo> &info) const;
    QString lastText(const std::shared_ptr<GroupInfo> &info) const;

    QVector<Row> _rows;
};

#endif // CONVERSATIONMODEL_H
