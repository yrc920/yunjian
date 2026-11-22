#ifndef MESSAGEMODEL_H
#define MESSAGEMODEL_H

#include <QAbstractListModel>
#include <QString>
#include <QVector>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <vector>

#include "userdata.h"

struct TextChatData;

/**
 * @brief 当前会话的消息模型
 *
 * 消息按 thread_id 绑定，单聊与群聊共用同一套模型；行数据直接读 UserMgr
 * 里对应会话的 _chat_msgs，不复制一份。切换会话时整表重置，收到/发出
 * 新消息时先做前缀比较，顺序没变就只插入或按行刷新。
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
        MsgIdRole, //客户端消息 ID；4014 之前还没有服务端 ID，视频/文件重试用它定位
        MsgTypeRole, //0 文本 / 1 图片 / 2 视频 / 3 文件 / 4 系统消息
        FileNameRole, //视频/文件的原始显示名
        FileSizeRole, //视频/文件字节数
        TransferStateRole, //ResourceTransferState，媒体卡片据此显示按钮
        TimeTextRole, //时间条文案；与上一条间隔不足 5 分钟时为空串，界面不渲染
        RecalledRole, //已撤回：只渲染一行提示，不显示气泡与资源
        CanRecallRole, //当前是否能撤回（自己发的、未撤回、在时间窗内）
        StatusTextRole, //「已读」/「未读」：只对自己发的最后一条显示，群聊不显示
        CanRetryRole, //发送失败：气泡旁给出可点击的重发入口
        RecallTextRole, //自己撤回的消息保留的原文，供「重新编辑」
        SenderNameRole, //群聊里显示在气泡上方；单聊与系统消息为空
        ShowSenderRole, //是否绘制发送者名字与头像（群里别人发的普通消息）
        SystemRole, //群系统消息：居中灰字，不画气泡
        MentionedRole, //这条消息 @ 了我，气泡高亮
        StartsGroupRole,
        KindRole, //会话类型：1 单聊 / 2 群聊
    };

    explicit MessageModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    //按行摊平成 map，字段与 roleNames 一致；会话页与调试都按它一次性读取
    Q_INVOKABLE QVariantMap get(int row) const;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // 切换到某个会话；thread_id 为 0 时清空。peer_uid 只在单聊里有值
    void setConversation(qint64 thread_id, int kind, int peer_uid);
    //消息窗口被按消息号重新装载（搜索结果跳转）后强制刷新整表
    void reload();
    //搜索结果跳转：按服务端消息号找行，找不到返回 -1
    Q_INVOKABLE int rowForMessage(qint64 message_id) const;

signals:
    void countChanged();

private:
    void onConversationChanged(qint64 thread_id, int peer_uid);
    //上滑翻到更早的一页：只在头部插入行，不重建整表，滚动位置由页面补偿
    void onMessagesPrepended(qint64 thread_id, int count);
    void syncCache(const std::vector<std::shared_ptr<TextChatData>>& msgs);
    std::shared_ptr<ChatThreadData> threadData() const;

    qint64 _threadId = 0;
    int _kind = static_cast<int>(ChatThreadKind::Private);
    int _peerUid = 0;
    int _cachedCount = 0;
    QVector<QString> _cachedIds;
    QVector<int> _cachedStatus;
};

#endif // MESSAGEMODEL_H
