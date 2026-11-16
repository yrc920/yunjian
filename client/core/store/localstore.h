#ifndef LOCALSTORE_H
#define LOCALSTORE_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <memory>
#include "singleton.h"

/******************************************************************************
 *
 * @file       localstore.h
 * @brief      本地缓存库：会话、消息、好友、群与成员的 SQLite 持久化
 *
 * 服务端仍是唯一权威：这里只保存"已经拿到过的数据"，用于重启后立刻可读、
 * 断线可翻历史、登录只拉增量。库文件按账号隔离，损坏时删掉重同步即可。
 *
 * 连接只允许在创建它的线程（主线程）使用，Qt 的 QSqlDatabase 不支持跨线程。
 *
 *****************************************************************************/

//一条消息在库里的形态：字段与 TextChatData 一一对应，瞬态字段（传输进度、
//本地路径）不入库，读回时由 ChatService 按资源名重新推导
struct StoredMessage {
    qint64 server_id = 0; //服务端消息号，未回执时为 0
    QString client_msg_id;
    qint64 thread_id = 0;
    int sender_id = 0;
    int recv_id = 0;
    int msg_type = 0;
    QString content;
    QString mentions;
    qint64 created_at = 0;
    int status = 1;
    bool is_read = false;
    bool recalled = false;
    QString recalled_text;
    QString resource_name;
    QString resource_md5;
    QString file_name;
    qint64 file_size = 0;
    QString thumb_name;
};

//会话行：last_* 是列表预览的冗余字段，避免启动时为每个会话各查一次消息
struct StoredThread {
    qint64 thread_id = 0;
    int thread_type = 1;
    int peer_uid = 0;
    qint64 last_time = 0;
    QString last_text;
    int last_type = 0;
    int last_sender = 0;
    QString last_name;
    int unread = 0;
    bool at_me = false;
    bool last_recalled = false;
    QString draft;
    qint64 sync_cursor = 0;
};

struct StoredFriend {
    int uid = 0;
    QString name;
    QString nick;
    QString remark;
    QString icon;
    int sex = 0;
    QString signature;
    bool is_friend = true;
    qint64 added_at = 0;
    QStringList tags;
};

struct StoredGroup {
    qint64 thread_id = 0;
    QString name;
    QString notice;
    int owner_id = 0;
    int my_role = 0;
    QString my_nick;
    bool muted = false;
    int left_state = 0;
    int member_count = 0;
    qint64 created_at = 0;
};

struct StoredMember {
    qint64 thread_id = 0;
    int uid = 0;
    int role = 0;
    QString group_nick;
    qint64 joined_at = 0;
    QString name;
    QString nick;
    QString icon;
    int sex = 0;
};

class LocalStore : public Singleton<LocalStore>
{
public:
    friend class Singleton<LocalStore>;
    ~LocalStore();

    //按账号打开 users/<uid>/chat.db；重复调用同一个 uid 直接复用连接
    bool Open(int uid);
    void Close(); //退出前调用：先 checkpoint 再关连接，避免 -wal 里留下数据
    bool IsOpen() const;
    int OwnerUid() const { return _uid; }
    QString DatabasePath() const { return _path; }

    // ---- 会话 ----
    void EnsureThread(qint64 thread_id, int thread_type, int peer_uid);
    //服务端只给出最后活动时间时用它：只前进不后退，避免把预览时间改小
    void SaveThreadTime(qint64 thread_id, qint64 last_time);
    void SaveThreadPreview(qint64 thread_id, qint64 last_time, const QString &text,
                           int type, int sender, const QString &file_name, bool recalled = false);
    //按库里最后一条消息重算预览：撤回、删除之后会话行不能还显示旧内容
    void RefreshThreadPreview(qint64 thread_id);
    void SaveThreadUnread(qint64 thread_id, int unread, bool at_me);
    void SaveThreadDraft(qint64 thread_id, const QString &html);
    QVector<StoredThread> LoadThreads() const;
    QString LoadDraft(qint64 thread_id) const;
    qint64 SyncCursor(qint64 thread_id) const;

    // ---- 消息 ----
    //一页消息与游标在同一个事务里提交：崩溃后不会出现"游标前进了、消息缺了一页"
    void SaveMessages(qint64 thread_id, int thread_type, int peer_uid,
                      const QVector<StoredMessage> &batch, qint64 cursor);
    void SaveMessage(qint64 thread_id, int thread_type, int peer_uid,
                     const StoredMessage &message);
    //回执：把本地占位行补上服务端消息号；冲突说明同步先落了库，删掉本地占位即可
    void UpdateMessageAck(qint64 thread_id, const QString &client_msg_id, qint64 server_id,
                          qint64 created_at, int status);
    void MarkRecalled(qint64 server_id, const QString &recalled_text);
    void MarkReadTo(qint64 thread_id, int self_uid, qint64 up_to_message_id);
    void DeleteMessage(qint64 thread_id, const QString &client_msg_id);
    //清空一个会话的消息但保留游标：清完不会在下一次登录又被全量拉回来
    void ClearThreadMessages(qint64 thread_id);
    //清空所有会话的消息，保留会话、好友、群与游标
    void ClearAllMessages();
    //取最近 limit 条（含未回执的本地消息），按消息号升序返回
    QVector<StoredMessage> LoadNewest(qint64 thread_id, int limit, bool *has_more) const;
    //取比 before_id 更早的一页，按消息号升序返回
    QVector<StoredMessage> LoadBefore(qint64 thread_id, qint64 before_id, int limit,
                                      bool *has_more) const;

    // ---- 好友与群 ----
    void SaveFriend(const StoredFriend &item);
    void SaveFriends(const QVector<StoredFriend> &items);
    void SetFriendActive(int uid, bool active); //删好友只置标志，历史会话仍可读
    void SaveFriendTags(int uid, const QStringList &tags);
    QVector<StoredFriend> LoadFriends() const;

    void SaveGroup(const StoredGroup &item);
    QVector<StoredGroup> LoadGroups() const;
    void SaveMembers(qint64 thread_id, const QVector<StoredMember> &members);
    void SaveMember(const StoredMember &member);
    QVector<StoredMember> LoadMembers() const;

private:
    LocalStore();

    bool openAt(int uid);  //建立连接并设置 PRAGMA，不含建表
    bool migrate();        //按 user_version 逐级建表，新库从 0 走到当前版本
    bool createSchemaV1();
    bool upgradeSchemaV2(); //v2：会话预览补 last_recalled，撤回后列表不再显示旧正文
    bool upgradeSchemaV3(); //v3：群补 left_state，被移出/解散后仍能保留历史
    void applyPragmas();
    //SQL 失败时把驱动给出的原始错误一起打出来：只打语句本身很难定位是语法还是约束
    void logError(const QString &sql) const;
    void logError(const QSqlQuery &query) const;

    QSqlDatabase _db;
    QString _path;
    int _uid = 0;
    bool _ready = false;
};

#endif // LOCALSTORE_H
