#ifndef FRIENDSERVICE_H
#define FRIENDSERVICE_H

#include <QObject>
#include <QString>
#include <QTimer>
#include "singleton.h"

/**
 * @brief 好友关系服务：搜索用户、发起申请、通过申请
 *
 * 只负责协议与结果翻译，界面通过信号拿到数据。
 */
class FriendService : public QObject, public Singleton<FriendService>
{
    Q_OBJECT

public:
    friend class Singleton<FriendService>;
    ~FriendService();

    // query 纯数字按 uid 查，否则按用户名查
    bool searchUser(const QString &query);
    // 发出好友申请；返回 false 表示当前连接或上一次申请还没结束
    bool applyFriend(int touid, const QString &applyName, const QString &backName);
    void authFriend(int touid, const QString &backName);

signals:
    void userFound(int uid, const QString &name, const QString &nick,
                   const QString &desc, const QString &icon, int sex);
    void searchFailed(const QString &message);
    // 申请回执：服务端确认写入 friend_apply 后成功，否则带上可展示的原因
    void applyFinished(bool ok, const QString &message);

private:
    FriendService();
    void finishApply(bool ok, const QString &message);

    bool _searching = false;
    bool _applying = false;
    QTimer _searchTimer; //搜索超时保护，避免浮层一直停在「正在搜索」
    QTimer _applyTimer;  //申请超时保护，避免按钮永远停在「发送中」
};

#endif // FRIENDSERVICE_H
