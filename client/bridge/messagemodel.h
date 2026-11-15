#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

struct TextChatData;

/**
 * @brief 当前会话的消息模型
 *
 * 行数据直接读 UserMgr 里该好友的 _chat_msgs，不复制一份。
 * 切换会话时整表重置；收到/发出新消息时按增量插入行。
 */
class MessageModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        TextRole = Qt::UserRole + 1,
        SelfRole,
        FromUidRole,
        ImageSourceRole,
        ImageWidthRole,
        ImageHeightRole,
        AvatarRole,
        NameRole,
        StatusRole,
        IsImageRole, //图片消息：没有本地文件时由气泡显示占位
        TransferProgressRole, //资源传输进度 0..100，-1 表示不显示
        MessageIdRole, //服务端消息 ID，视频/文件操作按它定位
        MsgIdRole, //客户端消息 ID；1035 之前还没有服务端 ID，视频/文件重试用它定位
        MsgTypeRole, //0 文本 / 1 图片 / 2 视频 / 3 文件
        FileNameRole, //视频/文件的原始显示名
        FileSizeRole, //视频/文件字节数
        TransferStateRole, //ResourceTransferState，媒体卡片据此显示按钮
        TimeTextRole, //时间条文案；与上一条间隔不足 5 分钟时为空串，界面不渲染
        RecalledRole, //已撤回：只渲染一行提示，不显示气泡与资源
        CanRecallRole, //当前是否能撤回（自己发的、未撤回、在时间窗内）
        StatusTextRole, //「已读」/「未读」：只对自己发的最后一条显示，其余为空
        CanRetryRole, //发送失败：气泡旁给出可点击的重发入口
        RecallTextRole, //自己撤回的消息保留的原文，供「重新编辑」
    };

    explicit MessageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 切换到某个好友的会话，uid 为 0 表示清空
    void setFriend(int uid);

signals:
    void countChanged();

private:
    void onFriendChatChanged(int uid);
    void syncCache(const std::vector<std::shared_ptr<TextChatData>>& msgs);

    int _uid = 0;
    int _cachedCount = 0;
    QVector<QString> _cachedIds;
    QVector<int> _cachedStatus;
};

#endif // MESSAGEMODEL_H
