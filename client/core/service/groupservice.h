#ifndef GROUPSERVICE_H
#define GROUPSERVICE_H

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include "singleton.h"

// 群聊的协议入口：建群、群资料、群设置与成员管理。
// 只负责组帧与回包归类；群数据统一落到 UserMgr，界面通过 bridge 读取。
class GroupService : public QObject, public Singleton<GroupService>
{
    Q_OBJECT

public:
    ~GroupService();
    //建群：name 为空时服务端用"群聊"，members 是勾选的好友 uid
    void CreateGroup(const QString &name, const QList<int> &members);
    //拉群资料与成员；同一个群在途时只发一次
    void RequestGroupInfo(qint64 thread_id);
    //修改群设置：action 取 name/notice/nickname/mute
    void UpdateGroup(qint64 thread_id, const QString &action, const QString &value);
    void InviteMembers(qint64 thread_id, const QList<int> &members);
    void RemoveMember(qint64 thread_id, int target);
    void SetAdmin(qint64 thread_id, int target, bool admin);
    void LeaveGroup(qint64 thread_id);
    void DismissGroup(qint64 thread_id);

signals:
    void createFinished(bool ok, qint64 threadId, const QString &message);
    void infoLoaded(bool ok, qint64 threadId, const QString &message);
    void updateFinished(bool ok, qint64 threadId, const QString &action, const QString &message);
    void memberOpFinished(bool ok, qint64 threadId, const QString &action, const QString &message);
    //服务端推来一个新群（被邀请/建群）：会话层据此拉一次历史
    void groupActivated(qint64 threadId);
    void groupRemoved(qint64 threadId, bool dismissed);

private:
    friend class Singleton<GroupService>;
    GroupService();

    //1068：把资料、成员数、系统消息分别落到 UserMgr
    void applyGroupChanged(const QJsonObject &data);
    void appendSystemMessage(qint64 thread_id, const QJsonObject &message);

    bool _creating = false; //一次只允许一个建群请求在途，避免回包串号
    int _createToken = 0; //超时回调用它丢弃过期的请求
    QSet<qint64> _pending_info; //正在拉资料的群，防止重复请求
};

#endif // GROUPSERVICE_H
