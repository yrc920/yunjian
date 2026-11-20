# 04 · 数据模型

数据分两层：

- **MySQL**：账号、好友、好友申请、会话、成员、消息与归档，是唯一权威。
- **Redis**：token、会话租约、实例注册、验证码、限流计数、用户资料缓存、上传进度。

客户端另有一份 **SQLite** 本地缓存，只保存已经拿到过的数据，按账号隔离；
它不是权威，损坏或删除后由服务端重新同步。

> `backup/xigu.sql`（含演示会话与消息）与 `backup/xigu_clean.sql`（仅账号、好友与一个空会话）
> 使用同一份表结构。修改字段时必须同时更新两份，否则洗库后会拿到旧结构。

---

## 4.1 MySQL

数据库 `xigu`，字符集 `utf8mb4 / utf8mb4_unicode_ci`。
后端通过 Boost.MySQL（经典协议 3306）访问，连接参数在各自 `config.ini` 的 `[Mysql]` 段。

### 4.1.1 `user`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `uid` | int, AUTO_INCREMENT | 业务用户 ID，主键 |
| `name` | varchar(64), UNIQUE | 用户名，2–20 字符 |
| `email` | varchar(254), UNIQUE | 登录账号，入口统一小写 |
| `pwd` | varchar(255) | Argon2id 哈希串，不存明文 |
| `nick` | varchar(64) | 昵称，空时界面回落到用户名 |
| `desc` | varchar(255) | 个性签名（SQL 中用反引号） |
| `sex` | tinyint unsigned | 0 未知 / 1 男 / 2 女 |
| `icon` | varchar(255) | 头像资源名；空表示用客户端内置头像 |

注册走 `查询 name/email → INSERT → LAST_INSERT_ID`，并发由两个唯一键兜底；
不再有 `user_id` 计数表或 `reg_user` 存储过程。

### 4.1.2 `friend`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `self_id` | int | 关系持有者 |
| `friend_id` | int | 好友 uid |
| `back` | varchar(64) | 本人给对方设的备注，空串表示没设 |
| `tags` | varchar(512) | JSON 字符串数组，最多 10 个、单个 12 字符 |

主键 `(self_id, friend_id)`，不再保留无业务意义的自增 `id`。
好友关系是**双向两行**：A 同意 B 的申请时同时插入 `(A,B)` 与 `(B,A)`；
两条 INSERT 固定按 uid 升序执行，避免双方同时同意时形成 AB/BA 循环等待。

备注与标签都是单方面的：A 的行只影响 A 看到的名字与分组。
客户端显示名按 **备注 > 昵称 > 用户名** 回落。

### 4.1.3 `friend_apply`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `id` | bigint unsigned, AUTO_INCREMENT | 主键，也是申请列表分页游标 |
| `from_uid` | int | 申请人 |
| `to_uid` | int | 被申请人 |
| `status` | tinyint unsigned | 0 待处理 / 1 已通过 / 2 已拒绝 |
| `descs` | varchar(255) | 申请留言；同意后作为申请方欢迎消息 |
| `back_name` | varchar(64) | 申请方给通过方设的备注 |
| `tags` | varchar(512) | 申请方给通过方选的分组 |

唯一键 `(from_uid,to_uid)`：重复申请走 `ON DUPLICATE KEY UPDATE`，覆盖留言/备注/标签并把
`status` 复位为 0。`(to_uid,id)` 与 `(from_uid,id)` 两个索引分别服务"我收到的"和"我发出的"列表。

### 4.1.4 `conversation`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `thread_id` | bigint unsigned, AUTO_INCREMENT | 会话 ID |
| `thread_type` | tinyint unsigned | 1 单聊 / 2 群聊 |
| `dm_key` | varchar(32), UNIQUE | 单聊去重键 `小uid:大uid`；群聊为 NULL |
| `name` | varchar(64) | 群名；单聊为空 |
| `owner_id` | int | 群主；单聊为 0 |
| `state` | tinyint unsigned | 0 正常 / 1 已解散 |
| `dissolved_at` | datetime | 解散时间，未解散为 NULL |
| `notice` | varchar(500) | 群公告 |
| `last_message_id` | bigint unsigned | 最后一条消息 ID，只前进 |
| `last_message_at` | timestamp | 最后活动时间，会话列表排序 |
| `created_at` | timestamp | 创建时间 |

索引 `(state,last_message_at,thread_id)` 与列表查询
`WHERE state=0 ORDER BY last_message_at DESC, thread_id DESC` 完全对齐。
群解散只把 `state` 置 1，成员与消息保留；所有群查询都带 `state=0` 过滤。

单聊用 `dm_key` 唯一键去重：先普通查询，没有才 `INSERT`，撞键说明并发请求已创建，
回滚后按 `dm_key` 读回对方的 `thread_id`。不使用 `SELECT ... FOR UPDATE` 占位，
避免查不到行时的间隙锁与 INSERT 形成死锁。

### 4.1.5 `conversation_member`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `thread_id` | bigint unsigned | 会话 |
| `uid` | int | 成员 |
| `role` | tinyint unsigned | 0 成员 / 1 管理员 / 2 群主 |
| `nickname` | varchar(32) | 我在本群的昵称 |
| `muted` | tinyint unsigned | 群免打扰 |
| `last_read_message_id` | bigint unsigned | 唯一已读游标，只前进 |
| `joined_at` | timestamp | 加入时间 |

主键 `(thread_id,uid)`，索引 `(uid,thread_id)` 服务"我的会话列表"。
未读数由 `COUNT(chat_message WHERE thread_id=? AND message_id>last_read_message_id AND sender_id<>me)`
推导；群聊的"有人@我"再用 `FIND_IN_SET(me, mentions)` 判断。
不保留逐条 `is_read` 列：单聊的已读由对端游标比较得出，群聊不展示逐条回执。

### 4.1.6 `chat_message`

| 列 | 类型 | 说明 |
| --- | --- | --- |
| `message_id` | bigint unsigned, AUTO_INCREMENT | 全局消息号，决定会话内顺序 |
| `thread_id` | bigint unsigned | 会话 |
| `sender_id` | int | 发送者 |
| `recv_id` | int | 单聊接收者；群聊固定 0（协议字段） |
| `client_msg_id` | varchar(64) | 客户端消息 ID，重发保持不变 |
| `content` | text | 正文；图片为资源名；视频/文件为元数据 JSON |
| `created_at` | timestamp | 落库时间，重发时保留原值 |
| `state` | tinyint unsigned | 0 正常 / 1 已撤回 |
| `upload_state` | tinyint unsigned | 0 资源未上传 / 1 已就绪 |
| `msg_type` | tinyint unsigned | 0 文本 / 1 图片 / 2 视频 / 3 文件 / 4 群系统消息 |
| `mentions` | varchar(2048) | 逗号分隔的 @ uid；2KB 请求内可容纳整群 200 人 |

- 幂等键 `(thread_id,sender_id,client_msg_id)`：重发命中唯一键时用
  `ON DUPLICATE KEY UPDATE message_id = LAST_INSERT_ID(message_id)` 取回原 ID 与时间。
- 索引 `(thread_id,message_id)` 服务历史分页；`(created_at,message_id)` 服务归档扫描。
- 外键 `thread_id → conversation.thread_id ON DELETE CASCADE`；群解散是软删，不会触发级联。
- 视频/文件的 `content`：
  `{"name":原始文件名,"size":字节数,"resource":资源名,"thumb":首帧资源名}`。
- `upload_state=0` 表示消息已落库、资源还在上传；ResourceServer 最后一片落盘后
  通过 gRPC 通知 ChatServer 改成 1，并对接收方推送 4012/4015。

已读与撤回不再用独立的 `is_read` / `recalled` 列：
撤回对应 `state=1`（正文清空、`msg_type` 归 0），已读由成员游标推导。

### 4.1.7 `chat_message_archive`

与 `chat_message` 同构，另有 `archived_at`，无外键。归档线程每 24 小时跑一次，
保留 365 天，每批 2000 条：先 `INSERT IGNORE ... SELECT`，再按同一截止时间 `DELETE`，
两个实例同时跑也只会搬不同的批次。归档表按 `message_id` 主键、`(thread_id,message_id)`
与 `created_at` 建索引。

---

## 4.2 关键查询

| 场景 | SQL 形状 | 说明 |
| --- | --- | --- |
| 登录会话列表 | `conversation JOIN conversation_member` + 未读/成员数子查询 | 首屏 50 条，复合游标 |
| 历史消息 | `WHERE thread_id=? AND message_id>? ORDER BY message_id LIMIT ?` | 走 `(thread_id,message_id)` |
| 发消息校验 | 成员 + 双向好友一次 `EXISTS` | 一条 SQL 同时挡越权与非好友 |
| 写消息 | 批量 `INSERT ... ON DUPLICATE KEY` + 按 `client_msg_id` 回读 | 一个事务，回执用库里时间 |
| 更新会话 | `UPDATE conversation ... WHERE last_message_id < ?` | 事务外执行，避免会话行死锁 |
| 已读 | `UPDATE conversation_member SET last_read_message_id=? WHERE ... AND last_read_message_id<?` | 只前进 |
| 归档 | `INSERT IGNORE` + `DELETE ... ORDER BY message_id LIMIT` | 分批，可重入 |

所有 DAO 通过 `common/MysqlSession` 执行：每条语句带 deadline（默认 5000ms），
超时用 Asio 操作级取消并标记连接污染，归还时丢弃；会话级还设置了
`max_execution_time=4000`、`innodb_lock_wait_timeout=3`、`lock_wait_timeout=10`。

---

## 4.3 Redis

连接池容量 10，1 秒有界借出；后台每 60 秒 PING 空闲连接并补池。

| 键 | 类型 | 写入方 | TTL | 说明 |
| --- | --- | --- | --- | --- |
| `code_<email>` | String | VerifyServer | 600s | 验证码 |
| `code_cd_<email>` | String | GateServer | 60s | 验证码冷却 |
| `code_hr_<email>_<bucket>` | String | GateServer | 3600s | 邮箱小时配额 |
| `code_ip_<ip>_<bucket>` | String | GateServer | 3600s | IP 小时配额 |
| `loginfail_<account>_<bucket>` | String | GateServer | 60s | 登录失败计数 |
| `loginfail_ip_<ip>_<bucket>` | String | GateServer | 60s | 登录失败 IP 计数 |
| `utoken_<uid>` | String | StatusServer | 7d | 登录令牌，登出即删 |
| `sess_<uid>` | Hash | StatusServer | 90s | 租约：`sid/inst/epoch`，每 30 秒续租 |
| `sess_rate_<uid>` | String | StatusServer | 10s | 登录频率计数 |
| `epoch_<uid>` | String | StatusServer | 无 | 围栏令牌，`max(计数+1, 毫秒)`，只增不减 |
| `inst_<name>` | Hash | StatusServer | 90s | 实例地址、端口、容量、负载、draining |
| `instances` | Set | StatusServer | 无 | 已注册实例名 |
| `ubaseinfo_<uid>` | String(JSON) | ChatServer / ResourceServer | 3600s | 用户公开资料 |
| `nameinfo_<name>` | String(JSON) | ChatServer / ResourceServer | 3600s | 用户名索引 |
| `file_upload_<md5>` | String(JSON) | ResourceServer | 3600s | 分片上传进度 |

用户资料缓存是 Cache-Aside：先读 Redis，未命中查 MySQL 并回填两个索引；
资料修改与头像上传都会主动覆盖，TTL 只是兜底。缓存里**不放密码哈希**。

`sess_` / `epoch_` / `inst_` 只由 StatusServer 写入；ChatServer 通过 RPC 查询，
不直接读写在线状态键。`utoken_` 由 StatusServer 写，ChatServer 与 ResourceServer 只读。

---

## 4.4 客户端 SQLite（本地缓存）

文件：`AppData/users/<uid>/chat.db`，按账号隔离，WAL 模式，`user_version` 逐级迁移。

| 表 | 用途 |
| --- | --- |
| `thread` | 会话行：类型、对方 uid、预览、未读、@我、草稿、同步游标、置顶/免打扰/隐藏 |
| `message` | 消息：服务端 ID、客户端 ID、类型、正文、时间、状态、已读、撤回、资源元数据 |
| `friend` | 好友与已删好友（`is_friend=0` 保留历史） |
| `grp` | 群资料与我的群设置 |
| `member` | 群成员资料 |

关键索引：`message(thread_id,server_id)` 服务窗口分页，
`message(thread_id,sender_id,client_msg_id) WHERE client_msg_id<>''` 服务本地幂等。
消息搜索用 `LIKE` 扫描本机会话；数据量在单机个人历史量级，暂不引入 FTS。
