#ifndef PROFILESERVICE_H
#define PROFILESERVICE_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include "singleton.h"

/**
 * @brief 资料服务：本人资料、好友备注与标签、好友最新资料拉取
 *
 * 与 FriendService 区分：那边管搜索与好友申请，这里管关系建立之后的资料编辑。
 * 只负责协议与结果翻译，成功后由 UserMgr 更新本地缓存，界面通过信号刷新。
 */
class ProfileService : public QObject, public Singleton<ProfileService>
{
    Q_OBJECT

public:
    friend class Singleton<ProfileService>;
    ~ProfileService();

    // 修改本人资料；返回 false 表示当前连接不可用或上一次请求还没结束
    bool updateProfile(const QString &nick, int sex, const QString &desc);
    // 修改好友备注与标签；空备注表示清除，空标签数组表示清空分组
    bool updateFriendRelation(int uid, const QString &remark, const QStringList &tags);
    // 拉取好友最新昵称与签名；轻量只读请求，不占用在途标记，失败时静默
    void requestProfile(int uid);

signals:
    // 本人资料保存结果，失败时 message 可直接展示
    void profileUpdated(bool ok, const QString &message);
    // 好友备注与标签保存结果，成功时带回服务端确认后的值
    void remarkUpdated(bool ok, int uid, const QString &remark, const QStringList &tags,
                       const QString &message);
    // 好友最新资料已写入 UserMgr；界面据此重建资料页快照与聊天标题
    void profileLoaded(int uid);

private:
    ProfileService();
    void finishProfile(bool ok, const QString &message);
    void finishRemark(bool ok, int uid, const QString &remark, const QStringList &tags,
                      const QString &message);

    bool _updating = false;
    bool _remarking = false;
    int _pendingRemarkUid = 0; //在途备注请求的目标，超时时用它结束界面等待
    QString _pendingRemark;
    QStringList _pendingTags;
    QTimer _profileTimer; //超时保护，避免按钮一直停在保存中
    QTimer _remarkTimer;
};

#endif // PROFILESERVICE_H
