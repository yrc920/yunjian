#include "localstore.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <algorithm>

namespace {
constexpr int kSchemaVersion = 3;
const char *kConnectionName = "xiguchat_local";

//所有消息查询共用一份列清单，保证字段顺序与 readMessage 一一对应
const char *kMessageColumns =
    "server_id, client_msg_id, thread_id, sender_id, recv_id, msg_type, content, "
    "mentions, created_at, status, is_read, recalled, recalled_text, "
    "resource_name, resource_md5, file_name, file_size, thumb_name";

StoredMessage readMessage(const QSqlQuery &query)
{    StoredMessage message;
    message.server_id = query.value(0).toLongLong();
    message.client_msg_id = query.value(1).toString();
    message.thread_id = query.value(2).toLongLong();
    message.sender_id = query.value(3).toInt();
    message.recv_id = query.value(4).toInt();
    message.msg_type = query.value(5).toInt();
    message.content = query.value(6).toString();
    message.mentions = query.value(7).toString();
    message.created_at = query.value(8).toLongLong();
    message.status = query.value(9).toInt();
    message.is_read = query.value(10).toBool();
    message.recalled = query.value(11).toBool();
    message.recalled_text = query.value(12).toString();
    message.resource_name = query.value(13).toString();
    message.resource_md5 = query.value(14).toString();
    message.file_name = query.value(15).toString();
    message.file_size = query.value(16).toLongLong();
    message.thumb_name = query.value(17).toString();
    return message;
}

//QString 默认构造是 null，Qt 会把它绑成 SQL NULL，碰到 NOT NULL 列直接整行失败。
//文本列统一走这里：null 与空串在业务上是同一个意思，落库时都写成空串
QVariant textValue(const QString &value)
{
    return QVariant(value.isNull() ? QStringLiteral("") : value);
}

}

LocalStore::LocalStore() = default;

LocalStore::~LocalStore()
{
    Close();
}

//按账号打开库。迁移失败说明库结构不兼容或已损坏：缓存不是权威数据，
//把旧文件挪到 .bak 后重建一份，服务端会在下次登录把历史补齐
bool LocalStore::Open(int uid)
{
    if (uid <= 0)
        return false;
    if (_ready && _uid == uid && _db.isOpen())
        return true;

    Close();
    if (!openAt(uid))
        return false;
    if (migrate()) {
        _ready = true;
        return true;
    }

    const QString broken = _path;
    Close();
    const QString backup = broken + QStringLiteral(".bak");
    QFile::remove(backup);
    QFile::remove(broken + QStringLiteral("-wal"));
    QFile::remove(broken + QStringLiteral("-shm"));
    if (!QFile::rename(broken, backup))
        qWarning() << "[store] 旧库备份失败" << broken;
    else
        qWarning() << "[store] 本地库不可用，已备份到" << backup;

    if (!openAt(uid) || !migrate()) {
        Close();
        return false;
    }
    _ready = true;
    return true;
}

//建目录、开连接、设 PRAGMA；建表交给 migrate，失败时可以整段重来
bool LocalStore::openAt(int uid)
{
    const QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) {
        qWarning() << "[store] 无法定位应用数据目录，本地缓存不可用";
        return false;
    }
    const QString dir = QDir(root).filePath(QStringLiteral("users/%1").arg(uid));
    if (!QDir().mkpath(dir)) {
        qWarning() << "[store] 创建账号目录失败" << dir;
        return false;
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        qWarning() << "[store] 缺少 QSQLITE 驱动，本地缓存不可用";
        return false;
    }

    _path = QDir(dir).filePath(QStringLiteral("chat.db"));
    _uid = uid;
    _db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                    QString::fromLatin1(kConnectionName));
    _db.setDatabaseName(_path);
    if (!_db.open()) {
        qWarning() << "[store] 打开数据库失败" << _path << _db.lastError().text();
        _db = QSqlDatabase();
        QSqlDatabase::removeDatabase(QString::fromLatin1(kConnectionName));
        _uid = 0;
        _path.clear();
        return false;
    }
    applyPragmas();
    return true;
}

void LocalStore::Close()
{
    if (!_db.isValid()) {
        _ready = false;
        _uid = 0;
        _path.clear();
        return;
    }
    const QString name = _db.connectionName();
    if (_db.isOpen()) {
        //WAL 里可能还有未合并的页；正常退出前合并一次，复制 .db 才是完整快照
        {
            QSqlQuery query(_db);
            query.exec(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
        }
        _db.close();
    }
    _db = QSqlDatabase();
    QSqlDatabase::removeDatabase(name);
    _ready = false;
    _uid = 0;
    _path.clear();
}

bool LocalStore::IsOpen() const
{
    return _ready && _db.isOpen();
}

//每次连接都要重设：PRAGMA 是连接级状态，不写进库文件
void LocalStore::applyPragmas()
{
    QSqlQuery query(_db);
    query.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    query.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    query.exec(QStringLiteral("PRAGMA foreign_keys = ON")); //默认关闭，删会话要靠它级联
    query.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
    query.exec(QStringLiteral("PRAGMA temp_store = MEMORY"));
}

//user_version 阶梯：新库从 0 一级级走到当前版本，升级只加步骤不改历史
bool LocalStore::migrate()
{
    QSqlQuery query(_db);
    if (!query.exec(QStringLiteral("PRAGMA user_version")) || !query.next()) {
        logError(query);
        return false;
    }
    int version = query.value(0).toInt();
    query.finish();

    if (version > kSchemaVersion) {
        qWarning() << "[store] 库版本高于程序" << version << kSchemaVersion;
        return false;
    }
    while (version < kSchemaVersion) {
        ++version;
        bool ok = false;
        switch (version) {
        case 1: ok = createSchemaV1(); break;
        case 2: ok = upgradeSchemaV2(); break;
        case 3: ok = upgradeSchemaV3(); break;
        default: ok = false; break;
        }
        if (!ok)
            return false;
        QSqlQuery bump(_db);
        if (!bump.exec(QStringLiteral("PRAGMA user_version = %1").arg(version))) {
            logError(bump);
            return false;
        }
    }
    return true;
}

bool LocalStore::createSchemaV1()
{
    //字段命名避开 SQL 保留字：签名用 signature，群用 grp
    const QStringList statements = {
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS thread ("
            " thread_id   INTEGER PRIMARY KEY,"
            " thread_type INTEGER NOT NULL DEFAULT 1,"
            " peer_uid    INTEGER NOT NULL DEFAULT 0,"
            " last_time   INTEGER NOT NULL DEFAULT 0,"
            " last_text   TEXT    NOT NULL DEFAULT '',"
            " last_type   INTEGER NOT NULL DEFAULT 0,"
            " last_sender INTEGER NOT NULL DEFAULT 0,"
            " last_name   TEXT    NOT NULL DEFAULT '',"
            " unread      INTEGER NOT NULL DEFAULT 0,"
            " at_me       INTEGER NOT NULL DEFAULT 0,"
            " draft       TEXT    NOT NULL DEFAULT '',"
            " sync_cursor INTEGER NOT NULL DEFAULT 0,"
            " synced_at   INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS message ("
            " id            INTEGER PRIMARY KEY,"
            " server_id     INTEGER UNIQUE,"
            " thread_id     INTEGER NOT NULL REFERENCES thread(thread_id) ON DELETE CASCADE,"
            " client_msg_id TEXT    NOT NULL DEFAULT '',"
            " sender_id     INTEGER NOT NULL DEFAULT 0,"
            " recv_id       INTEGER NOT NULL DEFAULT 0,"
            " msg_type      INTEGER NOT NULL DEFAULT 0,"
            " content       TEXT    NOT NULL DEFAULT '',"
            " mentions      TEXT    NOT NULL DEFAULT '',"
            " created_at    INTEGER NOT NULL DEFAULT 0,"
            " status        INTEGER NOT NULL DEFAULT 1,"
            " is_read       INTEGER NOT NULL DEFAULT 0,"
            " recalled      INTEGER NOT NULL DEFAULT 0,"
            " recalled_text TEXT    NOT NULL DEFAULT '',"
            " resource_name TEXT    NOT NULL DEFAULT '',"
            " resource_md5  TEXT    NOT NULL DEFAULT '',"
            " file_name     TEXT    NOT NULL DEFAULT '',"
            " file_size     INTEGER NOT NULL DEFAULT 0,"
            " thumb_name    TEXT    NOT NULL DEFAULT '')"),
        //群系统消息没有客户端消息号，只能约束真正由客户端产生的消息
        QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS uk_message_client "
            "ON message(thread_id, sender_id, client_msg_id) WHERE client_msg_id <> ''"),
        QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_message_thread_server "
            "ON message(thread_id, server_id)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS friend ("
            " uid       INTEGER PRIMARY KEY,"
            " name      TEXT    NOT NULL DEFAULT '',"
            " nick      TEXT    NOT NULL DEFAULT '',"
            " remark    TEXT    NOT NULL DEFAULT '',"
            " icon      TEXT    NOT NULL DEFAULT '',"
            " sex       INTEGER NOT NULL DEFAULT 0,"
            " signature TEXT    NOT NULL DEFAULT '',"
            " is_friend INTEGER NOT NULL DEFAULT 1,"
            " added_at  INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS friend_tag ("
            " uid INTEGER NOT NULL REFERENCES friend(uid) ON DELETE CASCADE,"
            " tag TEXT    NOT NULL,"
            " PRIMARY KEY (uid, tag))"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS grp ("
            " thread_id    INTEGER PRIMARY KEY,"
            " name         TEXT    NOT NULL DEFAULT '',"
            " notice       TEXT    NOT NULL DEFAULT '',"
            " owner_id     INTEGER NOT NULL DEFAULT 0,"
            " my_role      INTEGER NOT NULL DEFAULT 0,"
            " my_nick      TEXT    NOT NULL DEFAULT '',"
            " muted        INTEGER NOT NULL DEFAULT 0,"
            " member_count INTEGER NOT NULL DEFAULT 0,"
            " created_at   INTEGER NOT NULL DEFAULT 0)"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS group_member ("
            " thread_id  INTEGER NOT NULL REFERENCES grp(thread_id) ON DELETE CASCADE,"
            " uid        INTEGER NOT NULL,"
            " role       INTEGER NOT NULL DEFAULT 0,"
            " group_nick TEXT    NOT NULL DEFAULT '',"
            " joined_at  INTEGER NOT NULL DEFAULT 0,"
            " name       TEXT    NOT NULL DEFAULT '',"
            " nick       TEXT    NOT NULL DEFAULT '',"
            " icon       TEXT    NOT NULL DEFAULT '',"
            " sex        INTEGER NOT NULL DEFAULT 0,"
            " PRIMARY KEY (thread_id, uid))")
    };

    for (const QString &sql : statements) {
        QSqlQuery query(_db);
        if (!query.exec(sql)) {
            logError(sql);
            return false;
        }
    }
    return true;
}

//v2：撤回后的会话预览要能显示"撤回了一条消息"，只靠正文清空区分不出来
bool LocalStore::upgradeSchemaV2()
{
    QSqlQuery query(_db);
    const QString sql = QStringLiteral(
        "ALTER TABLE thread ADD COLUMN last_recalled INTEGER NOT NULL DEFAULT 0");
    if (!query.exec(sql)) {
        logError(sql);
        return false;
    }
    return true;
}

//v3：群离开状态。被移出或解散后仍要能打开历史，只靠"群不存在"区分不出来
bool LocalStore::upgradeSchemaV3()
{
    QSqlQuery query(_db);
    const QString sql = QStringLiteral(
        "ALTER TABLE grp ADD COLUMN left_state INTEGER NOT NULL DEFAULT 0");
    if (!query.exec(sql)) {
        logError(sql);
        return false;
    }
    return true;
}

void LocalStore::logError(const QString &sql) const
{
    qWarning() << "[store]" << (_db.isValid() ? _db.lastError().text() : QString())
               << sql;
}

void LocalStore::logError(const QSqlQuery &query) const
{
    qWarning() << "[store]" << query.lastError().text() << query.lastQuery();
}

// ---- 会话 ----

void LocalStore::EnsureThread(qint64 thread_id, int thread_type, int peer_uid)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    QSqlQuery query(_db);
    //只补行，不覆盖已有类型：预览、未读这些调用点并不知道会话是单聊还是群聊，
    //让它们带上默认值去写会把群会话改成单聊
    query.prepare(QStringLiteral(
        "INSERT INTO thread(thread_id, thread_type, peer_uid) VALUES(?, ?, ?) "
        "ON CONFLICT(thread_id) DO NOTHING"));
    query.addBindValue(thread_id);
    query.addBindValue(thread_type);
    query.addBindValue(peer_uid);
    if (!query.exec())
        logError(query);
}

//只推进最后活动时间：服务端下发的时间可能比本地预览旧（本地已经收到更新的消息）
void LocalStore::SaveThreadTime(qint64 thread_id, qint64 last_time)
{
    if (!IsOpen() || thread_id <= 0 || last_time <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "UPDATE thread SET last_time = CASE WHEN ? > last_time THEN ? ELSE last_time END "
        "WHERE thread_id = ?"));
    query.addBindValue(last_time);
    query.addBindValue(last_time);
    query.addBindValue(thread_id);
    if (!query.exec())
        logError(query);
}

//列表预览：只保留最后一条的形状，足够渲染"谁说了什么"，不必存整条消息
void LocalStore::SaveThreadPreview(qint64 thread_id, qint64 last_time, const QString &text,
                                   int type, int sender, const QString &file_name, bool recalled)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "UPDATE thread SET last_time = ?, last_text = ?, last_type = ?, "
        "last_sender = ?, last_name = ?, last_recalled = ? WHERE thread_id = ?"));
    query.addBindValue(last_time);
    query.addBindValue(textValue(text));
    query.addBindValue(type);
    query.addBindValue(sender);
    query.addBindValue(textValue(file_name));
    query.addBindValue(recalled ? 1 : 0);
    query.addBindValue(thread_id);
    if (!query.exec())
        logError(query);
}

//撤回与删除之后重算预览：库里的最后一条才是会话行该显示的内容。
//不做这一步，重启后列表会继续显示已经撤回或已被删除的正文
void LocalStore::RefreshThreadPreview(qint64 thread_id)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "SELECT msg_type, content, sender_id, created_at, file_name, recalled "
        "FROM message WHERE thread_id = ? "
        "ORDER BY (server_id IS NULL) DESC, server_id DESC, id DESC LIMIT 1"));
    query.addBindValue(thread_id);
    if (!query.exec()) {
        logError(query);
        return;
    }
    if (!query.next()) {
        QSqlQuery clear(_db);
        clear.prepare(QStringLiteral(
            "UPDATE thread SET last_time = 0, last_text = '', last_type = 0, "
            "last_sender = 0, last_name = '', last_recalled = 0 WHERE thread_id = ?"));
        clear.addBindValue(thread_id);
        if (!clear.exec())
            logError(clear);
        return;
    }
    const int type = query.value(0).toInt();
    const QString content = query.value(1).toString();
    const int sender = query.value(2).toInt();
    const qint64 time = query.value(3).toLongLong();
    const QString file_name = query.value(4).toString();
    const bool recalled = query.value(5).toBool();
    query.finish();
    SaveThreadPreview(thread_id, time, content, type, sender, file_name, recalled);
}

//未读只在库里做算术：读-改-写交给 SQLite，避免并发覆盖
void LocalStore::SaveThreadUnread(qint64 thread_id, int unread, bool at_me)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "UPDATE thread SET unread = ?, at_me = ? WHERE thread_id = ?"));
    query.addBindValue(qMax(0, unread));
    query.addBindValue(at_me ? 1 : 0);
    query.addBindValue(thread_id);
    if (!query.exec())
        logError(query);
}

void LocalStore::SaveThreadDraft(qint64 thread_id, const QString &html)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral("UPDATE thread SET draft = ? WHERE thread_id = ?"));
    query.addBindValue(textValue(html));
    query.addBindValue(thread_id);
    if (!query.exec())
        logError(query);
}

QString LocalStore::LoadDraft(qint64 thread_id) const
{
    if (!IsOpen() || thread_id <= 0)
        return {};
    QSqlQuery query(_db);
    query.prepare(QStringLiteral("SELECT draft FROM thread WHERE thread_id = ?"));
    query.addBindValue(thread_id);
    if (!query.exec() || !query.next())
        return {};
    return query.value(0).toString();
}


QVector<StoredThread> LocalStore::LoadThreads() const
{
    QVector<StoredThread> threads;
    if (!IsOpen())
        return threads;
    QSqlQuery query(_db);
    if (!query.exec(QStringLiteral(
            "SELECT thread_id, thread_type, peer_uid, last_time, last_text, last_type, "
            "last_sender, last_name, unread, at_me, draft, sync_cursor, last_recalled "
            "FROM thread"))) {
        logError(query);
        return threads;
    }
    while (query.next()) {
        StoredThread thread;
        thread.thread_id = query.value(0).toLongLong();
        thread.thread_type = query.value(1).toInt();
        thread.peer_uid = query.value(2).toInt();
        thread.last_time = query.value(3).toLongLong();
        thread.last_text = query.value(4).toString();
        thread.last_type = query.value(5).toInt();
        thread.last_sender = query.value(6).toInt();
        thread.last_name = query.value(7).toString();
        thread.unread = query.value(8).toInt();
        thread.at_me = query.value(9).toBool();
        thread.draft = query.value(10).toString();
        thread.sync_cursor = query.value(11).toLongLong();
        thread.last_recalled = query.value(12).toBool();
        threads.push_back(thread);
    }
    return threads;
}

qint64 LocalStore::SyncCursor(qint64 thread_id) const
{
    if (!IsOpen() || thread_id <= 0)
        return 0;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral("SELECT sync_cursor FROM thread WHERE thread_id = ?"));
    query.addBindValue(thread_id);
    if (!query.exec() || !query.next())
        return 0;
    return query.value(0).toLongLong();
}

// ---- 消息 ----

//一页消息与游标同事务：要么整页可见，要么完全回到调用前，游标不会越过缺口
void LocalStore::SaveMessages(qint64 thread_id, int thread_type, int peer_uid,
                              const QVector<StoredMessage> &batch, qint64 cursor)
{
    if (!IsOpen() || thread_id <= 0 || batch.isEmpty())
        return;
    EnsureThread(thread_id, thread_type, peer_uid);
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }

    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "INSERT INTO message(server_id, thread_id, client_msg_id, sender_id, recv_id, "
        "msg_type, content, mentions, created_at, status, is_read, recalled, recalled_text, "
        "resource_name, resource_md5, file_name, file_size, thumb_name) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT DO UPDATE SET "
        "server_id = COALESCE(excluded.server_id, message.server_id),"
        "client_msg_id = excluded.client_msg_id,"
        "sender_id = excluded.sender_id,"
        "recv_id = excluded.recv_id,"
        "msg_type = excluded.msg_type,"
        "content = excluded.content,"
        "mentions = excluded.mentions,"
        "created_at = excluded.created_at,"
        "status = excluded.status,"
        "is_read = excluded.is_read,"
        "recalled = excluded.recalled,"
        "recalled_text = COALESCE(NULLIF(excluded.recalled_text, ''), message.recalled_text),"
        "resource_name = excluded.resource_name,"
        "resource_md5 = excluded.resource_md5,"
        "file_name = excluded.file_name,"
        "file_size = excluded.file_size,"
        "thumb_name = excluded.thumb_name"));

    qint64 max_id = 0;
    bool ok = true;
    for (const StoredMessage &message : batch) {
        //位置绑定必须按序号覆盖：addBindValue 会累加，第二次 exec 就会参数个数不符
        query.bindValue(0, message.server_id > 0 ? QVariant(message.server_id) : QVariant());
        query.bindValue(1, thread_id);
        query.bindValue(2, textValue(message.client_msg_id));
        query.bindValue(3, message.sender_id);
        query.bindValue(4, message.recv_id);
        query.bindValue(5, message.msg_type);
        query.bindValue(6, textValue(message.content));
        query.bindValue(7, textValue(message.mentions));
        query.bindValue(8, message.created_at);
        query.bindValue(9, message.status);
        query.bindValue(10, message.is_read ? 1 : 0);
        query.bindValue(11, message.recalled ? 1 : 0);
        query.bindValue(12, textValue(message.recalled_text));
        query.bindValue(13, textValue(message.resource_name));
        query.bindValue(14, textValue(message.resource_md5));
        query.bindValue(15, textValue(message.file_name));
        query.bindValue(16, message.file_size);
        query.bindValue(17, textValue(message.thumb_name));
        if (!query.exec()) {
            logError(query);
            ok = false;
            break;
        }
        max_id = qMax(max_id, message.server_id);
    }

    if (ok) {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        QSqlQuery bump(_db);
        bump.prepare(QStringLiteral(
            "UPDATE thread SET sync_cursor = CASE WHEN ? > sync_cursor THEN ? ELSE sync_cursor END,"
            " synced_at = ? WHERE thread_id = ?"));
        bump.addBindValue(cursor > 0 ? cursor : max_id);
        bump.addBindValue(cursor > 0 ? cursor : max_id);
        bump.addBindValue(now);
        bump.addBindValue(thread_id);
        if (!bump.exec()) {
            logError(bump);
            ok = false;
        }
    }

    if (ok)
        _db.commit();
    else
        _db.rollback();
}

void LocalStore::SaveMessage(qint64 thread_id, int thread_type, int peer_uid,
                             const StoredMessage &message)
{
    SaveMessages(thread_id, thread_type, peer_uid, QVector<StoredMessage>{message},
                 message.server_id);
}

//回执补号：先尝试就地更新占位行；若唯一索引冲突（同步已先落库），
//留下同步那条、删掉本地占位，避免出现两条同样的消息
void LocalStore::UpdateMessageAck(qint64 thread_id, const QString &client_msg_id,
                                  qint64 server_id, qint64 created_at, int status)
{
    if (!IsOpen() || thread_id <= 0 || client_msg_id.isEmpty())
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery update(_db);
    update.prepare(QStringLiteral(
        "UPDATE OR IGNORE message SET server_id = ?, created_at = ?, status = ? "
        "WHERE thread_id = ? AND client_msg_id = ? AND server_id IS NULL"));
    update.addBindValue(server_id);
    update.addBindValue(created_at);
    update.addBindValue(status);
    update.addBindValue(thread_id);
    update.addBindValue(textValue(client_msg_id));
    const bool ok = update.exec();
    if (!ok)
        logError(update);

    QSqlQuery cleanup(_db);
    cleanup.prepare(QStringLiteral(
        "DELETE FROM message WHERE thread_id = ? AND client_msg_id = ? AND server_id IS NULL"));
    cleanup.addBindValue(thread_id);
    cleanup.addBindValue(textValue(client_msg_id));
    if (!cleanup.exec())
        logError(cleanup);

    if (ok)
        _db.commit();
    else
        _db.rollback();
}


//撤回后正文与资源名一起清空：本地副本不该继续保留别人已经撤回的内容
void LocalStore::MarkRecalled(qint64 server_id, const QString &recalled_text)
{
    if (!IsOpen() || server_id <= 0)
        return;
    //先取所属会话：撤回的通知只带消息号，刷新预览需要 thread_id
    qint64 thread_id = 0;
    {
        QSqlQuery find(_db);
        find.prepare(QStringLiteral("SELECT thread_id FROM message WHERE server_id = ?"));
        find.addBindValue(server_id);
        if (find.exec() && find.next())
            thread_id = find.value(0).toLongLong();
    }

    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "UPDATE message SET recalled = 1, content = '', resource_name = '', "
        "resource_md5 = '', file_name = '', file_size = 0, thumb_name = '', "
        "recalled_text = COALESCE(NULLIF(?, ''), recalled_text) WHERE server_id = ?"));
    query.addBindValue(textValue(recalled_text));
    query.addBindValue(server_id);
    if (!query.exec()) {
        logError(query);
        return;
    }
    if (thread_id > 0)
        RefreshThreadPreview(thread_id);
}

void LocalStore::MarkReadTo(qint64 thread_id, int self_uid, qint64 up_to_message_id)
{
    if (!IsOpen() || thread_id <= 0 || self_uid <= 0 || up_to_message_id <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "UPDATE message SET status = 4, is_read = 1 "
        "WHERE thread_id = ? AND sender_id = ? AND server_id IS NOT NULL AND server_id <= ?"));
    query.addBindValue(thread_id);
    query.addBindValue(self_uid);
    query.addBindValue(up_to_message_id);
    if (!query.exec())
        logError(query);
}

void LocalStore::DeleteMessage(qint64 thread_id, const QString &client_msg_id)
{
    if (!IsOpen() || thread_id <= 0 || client_msg_id.isEmpty())
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "DELETE FROM message WHERE thread_id = ? AND client_msg_id = ?"));
    query.addBindValue(thread_id);
    query.addBindValue(textValue(client_msg_id));
    if (!query.exec()) {
        logError(query);
        return;
    }
    //删的正好是最后一条时，会话行的预览必须跟着回退到上一条
    RefreshThreadPreview(thread_id);
}

//清空一个会话的消息，但保留 sync_cursor 与 last_time：
//游标留着，下一次登录不会把刚清掉的历史又全量拉回来；时间留着，会话行不会突然沉到列表底部
void LocalStore::ClearThreadMessages(qint64 thread_id)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery remove(_db);
    remove.prepare(QStringLiteral("DELETE FROM message WHERE thread_id = ?"));
    remove.addBindValue(thread_id);
    bool ok = remove.exec();
    if (!ok)
        logError(remove);

    QSqlQuery preview(_db);
    preview.prepare(QStringLiteral(
        "UPDATE thread SET last_text = '', last_type = 0, last_sender = 0, "
        "last_name = '', last_recalled = 0, draft = '' WHERE thread_id = ?"));
    preview.addBindValue(thread_id);
    if (!preview.exec()) {
        logError(preview);
        ok = false;
    }

    if (ok)
        _db.commit();
    else
        _db.rollback();
}

//清空全部消息：会话、好友、群与游标都保留，只是历史没了
void LocalStore::ClearAllMessages()
{
    if (!IsOpen())
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery remove(_db);
    bool ok = remove.exec(QStringLiteral("DELETE FROM message"));
    if (!ok)
        logError(remove);

    QSqlQuery preview(_db);
    if (!preview.exec(QStringLiteral(
            "UPDATE thread SET last_text = '', last_type = 0, last_sender = 0, "
            "last_name = '', last_recalled = 0"))) {
        logError(preview);
        ok = false;
    }

    if (ok)
        _db.commit();
    else
        _db.rollback();
}

//最新一页：未回执的消息排在末尾（与内存里的排序规则一致），再整体反转成升序
QVector<StoredMessage> LocalStore::LoadNewest(qint64 thread_id, int limit, bool *has_more) const
{
    QVector<StoredMessage> messages;
    if (has_more)
        *has_more = false;
    if (!IsOpen() || thread_id <= 0 || limit <= 0)
        return messages;

    QSqlQuery query(_db);
    query.prepare(QStringLiteral("SELECT %1 FROM message WHERE thread_id = ? "
                                 "ORDER BY (server_id IS NULL) DESC, server_id DESC, id DESC "
                                 "LIMIT ?").arg(QString::fromLatin1(kMessageColumns)));
    query.addBindValue(thread_id);
    query.addBindValue(limit + 1);
    if (!query.exec()) {
        logError(query);
        return messages;
    }
    while (query.next())
        messages.push_back(readMessage(query));
    if (messages.size() > limit) {
        messages.removeLast();
        if (has_more)
            *has_more = true;
    }
    std::reverse(messages.begin(), messages.end());
    return messages;
}

//比 before_id 更早的一页；未回执的消息永远在末尾，不参与向前翻页
QVector<StoredMessage> LocalStore::LoadBefore(qint64 thread_id, qint64 before_id, int limit,
                                              bool *has_more) const
{
    QVector<StoredMessage> messages;
    if (has_more)
        *has_more = false;
    if (!IsOpen() || thread_id <= 0 || before_id <= 0 || limit <= 0)
        return messages;

    QSqlQuery query(_db);
    query.prepare(QStringLiteral("SELECT %1 FROM message WHERE thread_id = ? "
                                 "AND server_id IS NOT NULL AND server_id < ? "
                                 "ORDER BY server_id DESC LIMIT ?")
                      .arg(QString::fromLatin1(kMessageColumns)));
    query.addBindValue(thread_id);
    query.addBindValue(before_id);
    query.addBindValue(limit + 1);
    if (!query.exec()) {
        logError(query);
        return messages;
    }
    while (query.next())
        messages.push_back(readMessage(query));
    if (messages.size() > limit) {
        messages.removeLast();
        if (has_more)
            *has_more = true;
    }
    std::reverse(messages.begin(), messages.end());
    return messages;
}

// ---- 好友与群 ----

void LocalStore::SaveFriend(const StoredFriend &item)
{
    if (!IsOpen() || item.uid <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "INSERT INTO friend(uid, name, nick, remark, icon, sex, signature, is_friend, added_at) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(uid) DO UPDATE SET "
        "name = excluded.name, nick = excluded.nick, remark = excluded.remark, "
        "icon = excluded.icon, sex = excluded.sex, signature = excluded.signature, "
        "is_friend = excluded.is_friend"));
    query.addBindValue(item.uid);
    query.addBindValue(textValue(item.name));
    query.addBindValue(textValue(item.nick));
    query.addBindValue(textValue(item.remark));
    query.addBindValue(textValue(item.icon));
    query.addBindValue(item.sex);
    query.addBindValue(textValue(item.signature));
    query.addBindValue(item.is_friend ? 1 : 0);
    query.addBindValue(item.added_at);
    if (!query.exec())
        logError(query);
}

void LocalStore::SaveFriends(const QVector<StoredFriend> &items)
{
    if (!IsOpen() || items.isEmpty())
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery clear(_db);
    bool ok = true;
    for (const StoredFriend &item : items) {
        SaveFriend(item);
        clear.prepare(QStringLiteral("DELETE FROM friend_tag WHERE uid = ?"));
        clear.addBindValue(item.uid);
        if (!clear.exec()) {
            logError(clear);
            ok = false;
            break;
        }
        QSqlQuery tag(_db);
        tag.prepare(QStringLiteral("INSERT OR IGNORE INTO friend_tag(uid, tag) VALUES(?, ?)"));
        for (const QString &value : item.tags) {
            if (value.isEmpty())
                continue;
            tag.bindValue(0, item.uid);
            tag.bindValue(1, textValue(value));
            if (!tag.exec()) {
                logError(tag);
                ok = false;
                break;
            }
        }
        if (!ok)
            break;
    }
    if (ok)
        _db.commit();
    else
        _db.rollback();
}

void LocalStore::SetFriendActive(int uid, bool active)
{
    if (!IsOpen() || uid <= 0)
        return;
    QSqlQuery query(_db);
    query.prepare(QStringLiteral("UPDATE friend SET is_friend = ? WHERE uid = ?"));
    query.addBindValue(active ? 1 : 0);
    query.addBindValue(uid);
    if (!query.exec())
        logError(query);
}

void LocalStore::SaveFriendTags(int uid, const QStringList &tags)
{
    if (!IsOpen() || uid <= 0)
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery clear(_db);
    clear.prepare(QStringLiteral("DELETE FROM friend_tag WHERE uid = ?"));
    clear.addBindValue(uid);
    bool ok = clear.exec();
    if (!ok)
        logError(clear);

    QSqlQuery insert(_db);
    insert.prepare(QStringLiteral("INSERT OR IGNORE INTO friend_tag(uid, tag) VALUES(?, ?)"));
    for (const QString &value : tags) {
        if (!ok || value.isEmpty())
            continue;
        insert.bindValue(0, uid);
        insert.bindValue(1, textValue(value));
        if (!insert.exec()) {
            logError(insert);
            ok = false;
        }
    }
    if (ok)
        _db.commit();
    else
        _db.rollback();
}

QVector<StoredFriend> LocalStore::LoadFriends() const
{
    QVector<StoredFriend> friends;
    if (!IsOpen())
        return friends;

    QHash<int, QStringList> tags;
    {
        QSqlQuery query(_db);
        if (query.exec(QStringLiteral("SELECT uid, tag FROM friend_tag ORDER BY uid, tag"))) {
            while (query.next())
                tags[query.value(0).toInt()].append(query.value(1).toString());
        }
    }

    QSqlQuery query(_db);
    if (!query.exec(QStringLiteral(
            "SELECT uid, name, nick, remark, icon, sex, signature, is_friend, added_at "
            "FROM friend ORDER BY added_at, uid"))) {
        logError(query);
        return friends;
    }
    while (query.next()) {
        StoredFriend item;
        item.uid = query.value(0).toInt();
        item.name = query.value(1).toString();
        item.nick = query.value(2).toString();
        item.remark = query.value(3).toString();
        item.icon = query.value(4).toString();
        item.sex = query.value(5).toInt();
        item.signature = query.value(6).toString();
        item.is_friend = query.value(7).toBool();
        item.added_at = query.value(8).toLongLong();
        item.tags = tags.value(item.uid);
        friends.push_back(item);
    }
    return friends;
}

void LocalStore::SaveGroup(const StoredGroup &item)
{
    if (!IsOpen() || item.thread_id <= 0)
        return;
    EnsureThread(item.thread_id, 2, 0);
    QSqlQuery query(_db);
    query.prepare(QStringLiteral(
        "INSERT INTO grp(thread_id, name, notice, owner_id, my_role, my_nick, muted, "
        "left_state, member_count, created_at) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id) DO UPDATE SET "
        "name = excluded.name, notice = excluded.notice, owner_id = excluded.owner_id, "
        "my_role = excluded.my_role, my_nick = excluded.my_nick, muted = excluded.muted, "
        "left_state = excluded.left_state, member_count = excluded.member_count, "
        "created_at = excluded.created_at"));
    query.addBindValue(item.thread_id);
    query.addBindValue(textValue(item.name));
    query.addBindValue(textValue(item.notice));
    query.addBindValue(item.owner_id);
    query.addBindValue(item.my_role);
    query.addBindValue(textValue(item.my_nick));
    query.addBindValue(item.muted ? 1 : 0);
    query.addBindValue(item.left_state);
    query.addBindValue(item.member_count);
    query.addBindValue(item.created_at);
    if (!query.exec())
        logError(query);
}


QVector<StoredGroup> LocalStore::LoadGroups() const
{
    QVector<StoredGroup> groups;
    if (!IsOpen())
        return groups;
    QSqlQuery query(_db);
    if (!query.exec(QStringLiteral(
            "SELECT thread_id, name, notice, owner_id, my_role, my_nick, muted, "
            "left_state, member_count, created_at FROM grp ORDER BY created_at, thread_id"))) {
        logError(query);
        return groups;
    }
    while (query.next()) {
        StoredGroup item;
        item.thread_id = query.value(0).toLongLong();
        item.name = query.value(1).toString();
        item.notice = query.value(2).toString();
        item.owner_id = query.value(3).toInt();
        item.my_role = query.value(4).toInt();
        item.my_nick = query.value(5).toString();
        item.muted = query.value(6).toBool();
        item.left_state = query.value(7).toInt();
        item.member_count = query.value(8).toInt();
        item.created_at = query.value(9).toLongLong();
        groups.push_back(item);
    }
    return groups;
}

void LocalStore::SaveMembers(qint64 thread_id, const QVector<StoredMember> &members)
{
    if (!IsOpen() || thread_id <= 0)
        return;
    if (!_db.transaction()) {
        logError(QStringLiteral("BEGIN"));
        return;
    }
    QSqlQuery clear(_db);
    clear.prepare(QStringLiteral("DELETE FROM group_member WHERE thread_id = ?"));
    clear.addBindValue(thread_id);
    bool ok = clear.exec();
    if (!ok)
        logError(clear);

    QSqlQuery insert(_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO group_member(thread_id, uid, role, group_nick, joined_at, "
        "name, nick, icon, sex) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id, uid) DO UPDATE SET "
        "role = excluded.role, group_nick = excluded.group_nick, name = excluded.name, "
        "nick = excluded.nick, icon = excluded.icon, sex = excluded.sex"));
    for (const StoredMember &member : members) {
        if (!ok || member.uid <= 0)
            continue;
        insert.bindValue(0, thread_id);
        insert.bindValue(1, member.uid);
        insert.bindValue(2, member.role);
        insert.bindValue(3, textValue(member.group_nick));
        insert.bindValue(4, member.joined_at);
        insert.bindValue(5, textValue(member.name));
        insert.bindValue(6, textValue(member.nick));
        insert.bindValue(7, textValue(member.icon));
        insert.bindValue(8, member.sex);
        if (!insert.exec()) {
            logError(insert);
            ok = false;
        }
    }
    if (ok)
        _db.commit();
    else
        _db.rollback();
}

void LocalStore::SaveMember(const StoredMember &member)
{
    if (!IsOpen() || member.thread_id <= 0 || member.uid <= 0)
        return;
    QSqlQuery insert(_db);
    insert.prepare(QStringLiteral(
        "INSERT INTO group_member(thread_id, uid, role, group_nick, joined_at, "
        "name, nick, icon, sex) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(thread_id, uid) DO UPDATE SET "
        "role = excluded.role, group_nick = excluded.group_nick, name = excluded.name, "
        "nick = excluded.nick, icon = excluded.icon, sex = excluded.sex"));
    insert.addBindValue(member.thread_id);
    insert.addBindValue(member.uid);
    insert.addBindValue(member.role);
    insert.addBindValue(textValue(member.group_nick));
    insert.addBindValue(member.joined_at);
    insert.addBindValue(textValue(member.name));
    insert.addBindValue(textValue(member.nick));
    insert.addBindValue(textValue(member.icon));
    insert.addBindValue(member.sex);
    if (!insert.exec())
        logError(insert);
}

QVector<StoredMember> LocalStore::LoadMembers() const
{
    QVector<StoredMember> members;
    if (!IsOpen())
        return members;
    QSqlQuery query(_db);
    if (!query.exec(QStringLiteral(
            "SELECT thread_id, uid, role, group_nick, joined_at, name, nick, icon, sex "
            "FROM group_member ORDER BY thread_id, uid"))) {
        logError(query);
        return members;
    }
    while (query.next()) {
        StoredMember member;
        member.thread_id = query.value(0).toLongLong();
        member.uid = query.value(1).toInt();
        member.role = query.value(2).toInt();
        member.group_nick = query.value(3).toString();
        member.joined_at = query.value(4).toLongLong();
        member.name = query.value(5).toString();
        member.nick = query.value(6).toString();
        member.icon = query.value(7).toString();
        member.sex = query.value(8).toInt();
        members.push_back(member);
    }
    return members;
}
