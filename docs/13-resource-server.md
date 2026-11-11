# 13 · ResourceServer

资源服务器负责头像与文件的分段传输：客户端把文件切成固定大小的分片，逐片上传/下载，
服务端把进度写进 Redis，断线或暂停后可以只补传未完成的部分。

它和 ChatServer 是两套独立的 TCP 服务：

- 端口独立（默认 9090），客户端建立两条长连接；
- 帧头不同：聊天是 2 字节 ID + 2 字节长度，资源是 2 字节 ID + 4 字节长度；
- 线程模型不同：资源服务没有单消费者逻辑队列，而是按会话分派到多个 `LogicWorker`，
  落盘再按文件名分派到多个 `FileWorker` / `DownloadWorker`。

---

## 13.1 进程与线程

| 线程 | 数量 | 职责 |
| --- | --- | --- |
| 主线程 | 1 | 运行 acceptor 所在的 `io_context`，处理信号与停服 |
| `IOContextPool` | CPU 核心数 | 每个线程一个 `io_context`，承载会话的异步收发 |
| `LogicWorker` | 4 | 按会话 UUID 哈希分派，解析 JSON、校验 token、维护进度、投递文件任务 |
| `FileWorker` | 4 | 按文件名哈希分派，Base64 解码、追加写盘、最后一片更新头像 |
| `DownloadWorker` | 2 | 按文件名哈希分派，按 seq 读取分片并 Base64 编码 |

分派规则解决了两个顺序问题：

- 同一连接的所有请求固定落到同一个 `LogicWorker`，分片不会并发解析；
- 同一文件的所有分片固定落到同一个 `FileWorker`，`seq=1` 截断、后续追加的顺序不会乱。

客户端同一时刻只允许一个分片在途，收到回包后才发下一片，因此不需要锁住整个文件。

任务结果通过 `ResultCallback`（`std::function<void(const Json::Value&)>`）从文件线程交回逻辑线程：
`FileWorker` / `DownloadWorker` 只填 `error` 和自己产出的字段（分片数据、偏移、是否最后一片），
回包字段由 `ResourceHandler` 里的回调补齐后发送。文件线程因此不接触协议细节，
后续增加新的资源类型时也不用改回调签名。

---

## 13.2 帧格式

```
 0        1        2        3        4        5        6 ...
+--------+--------+--------+--------+--------+--------+------------------+
|     msg_id      |              msg_len             |      payload      |
|  (2 bytes, BE)  |          (4 bytes, BE)           |  (msg_len bytes)  |
+--------+--------+--------+--------+--------+--------+-------------------+
|<-------------- HEAD_TOTAL_LEN = 6 ---------------->|
```

| 常量 | 值 | 说明 |
| --- | --- | --- |
| `HEAD_TOTAL_LEN` | 6 | 2 字节 ID + 4 字节长度 |
| `MAX_FILE_LEN` | 2048 | 每个分片的原始字节数，两端必须一致 |
| `MAX_MESSAGE_LEN` | 16384 | 单帧包体上限；2KB 原始数据经 Base64 与 JSON 包装后仍有充足余量 |
| `MAX_FILE_SIZE` | 4GB | 单个文件上限 |

客户端 `FileTcpMgr` 使用同一套 6 字节头；聊天 `TcpMgr` 仍保持 4 字节头，两者不能混用。

---

## 13.3 消息 ID

| ID | 方向 | 用途 |
| --- | --- | --- |
| 2001 / 2002 | Client → Server / 回包 | 通用文件分段上传 |
| 2003 / 2004 | Client → Server / 回包 | 续传前同步服务器进度 |
| 2005 / 2006 | Client → Server / 回包 | 断点下载 |
| 2007 / 2008 | Client → Server / 回包 | 头像分段上传 |

定义在 `services/ResourceServer/include/const.h` 与 `client/core/global.h` 的 `ReqId`，
新增/修改协议时两端必须同步。

### 上传请求

```json
{
  "uid": 1006,
  "token": "utoken_<uid> 对应的值",
  "md5": "32位十六进制",
  "name": "<md5>.jpg（透明图用 .png）",
  "seq": 1,
  "trans_size": 2048,
  "total_size": 31975,
  "last": 0,
  "data": "Base64 分片"
}
```

服务端检查：

- `uid`、`token` 与 Redis `utoken_<uid>` 一致；第一个请求通过后把 uid 绑到连接，
  后续分片只校验连接身份，不再重复访问 Redis；
- `md5` 为 32 位十六进制，`name` 不能包含 `/`、`\` 或以 `.` 开头；
- `total_size ≤ 4GB`，`trans_size = (seq-1) × 2048 + 分片长度`；
- `last == 1` 必须与 `trans_size == total_size` 同时成立；
- `seq > 1` 时，服务器必须已有该 md5 的进度，且 `seq` 必须等于上一片序号 + 1。

任一项不满足都会返回对应错误码，不会写盘。回包字段：

```json
{
  "error": 0,
  "md5": "...",
  "name": "...",
  "seq": 1,
  "trans_size": 2048,
  "total_size": 31975,
  "last": 0,
  "uid": 1006
}
```

### 同步进度

```json
{ "uid": 1006, "token": "...", "md5": "..." }
```

回包带 `seq`、`trans_size`、`total_size`。客户端按 `trans_size` 偏移读取下一片，
`seq` 直接用服务器值 + 1，避免本地进度落后时重复追加。

### 下载请求

```json
{ "uid": 1006, "token": "...", "owner_uid": 1006, "name": "<md5>.png", "seq": 1 }
```

`uid` 是请求者，用于校验 token；`owner_uid` 是文件属于谁。回包：

```json
{
  "error": 0,
  "name": "<md5>.png",
  "seq": 1,
  "data": "Base64 分片",
  "is_last": false,
  "current_size": 2048,
  "total_size": 31975
}
```

`seq` 从 1 开始，`is_last` 为 true 时客户端停止请求。头像允许读取其他用户目录，
因此好友头像也能通过同一接口下载；token 只校验请求者身份。

错误码：文件不存在返回 `1012`，`seq` 对应的偏移越过文件尾返回 `1020`，
源文件打不开或读失败返回 `1019`。客户端收到任何非 0 都会结束本次下载并清理半截缓存。

---

## 13.4 进度与存储

| 位置 | 键 / 路径 | 内容 |
| --- | --- | --- |
| Redis | `file_upload_<md5>` | `file_path`、`name`、`uid`、`seq`、`total_size`、`trans_size`、`avatar`；`SETEX` 3600 秒 |
| 磁盘 | `<Storage>/<uid>/<name>` | 头像与通用文件都按上传者分目录，下载用 `owner_uid` 拼出同一路径 |
| MySQL | `user.icon` | 最后一片落盘后写入文件名 |
| Redis | `ubaseinfo_<uid>`、`nameinfo_<name>` | 同步更新 icon，ChatServer 登录直接读取 |

内存里也保存一份进度（`LogicSystem::_file_infos`），Redis 用于跨连接/换连接续传。
最后一片完成后从内存移除，Redis 保留一小时供客户端做最终同步。

通用资源（聊天图片）使用拥塞窗口，多个分片可能在途。`FileInfo` 额外维护内存字段
`_accepted_seq`（逻辑线程已接收的序号）：处理器按它校验并推进，可领先于落盘进度；
文件线程按同一文件的队列顺序写盘，进度只前进不后退。`_accepted_seq` 不写 Redis，
从 Redis 恢复时等于已落盘的 `_seq`。

写盘顺序是「先写文件，再更新进度」，回包在写盘完成后才发出。因此
`trans_size` 表示已经落盘的字节数，不是客户端声称的进度。

最后一片写完后，如果请求携带 `message_id`（聊天图片），资源服务器通过 gRPC
调用 ChatServer 的 `NotifyImageUploaded`，把 `chat_message.status` 从 0（未上传）
改成 1（已上传）；gRPC 不可用时回退为直接更新数据库。

---

## 13.5 客户端断点续传

`client/network/filetcpmgr.{h,cpp}`：

- 独立 `FileTcpThread` + `FileTcpMgr`，不与聊天 `TcpMgr` 共用线程；
- 发送队列用 `bytesWritten` 续写，处理 `socket.write()` 只写出一部分的情况；
- 上传：`UserMgr` 保存 `UploadFileInfo`（路径、md5、总大小、req_id 与窗口状态），
  聊天图片最多 8 片在途，头像固定 1 片；`seq` 从 1 开始，收到 2008/2002 后按确认序号补发；
- 进度按“连续确认到的分片”计算，乱序回包不会让进度条回跳；全部确认后发 `sig_upload_finished`；
- 聊天图片由 `ChatService` 把输入区压缩图（长边 1600 的 JPEG/PNG）复制到本机缓存后复用 `2001`
  通道，与头像上传的状态互不影响；`FileService` 按资源名把进度与完成/失败转发给消息行，
  上传完成后消息状态变为已发送，进度条隐藏；
- 下载：`DownloadFileInfo` 保存本地路径与 seq，`seq == 1` 覆盖写，否则追加；
- 暂停：头像窗口为 1，置 `UserMgr::_upload_paused` 后当前分片回包即停止；聊天图片不受影响；
- 继续：向服务器发 2003，按返回的 `seq` 重建连续确认位置后继续。

`client/core/service/fileservice.{h,cpp}` 负责业务编排：

- `UploadAvatar`：校验 PNG、计算 MD5，建立上传状态并启动第一片；
- `OnSessionReady`：登录完成后拉取本机、好友与好友申请里缺失的头像，已有缓存直接使用；
- 下载完成后，本机头像写回 `LocalAvatarPath`，好友/申请人头像写回
  `AppDataLocation/avatars/cache/<网关哈希>/<uid>/<name>` 并刷新对应列表；
- 新头像开始上传前取消本机旧头像的下载，避免服务端以 `seq=1` 截断文件后
  旧下载把本机头像覆盖成残缺内容。
- 同一个 uid 可能同时出现在好友列表和「新的朋友」：`UserMgr::SetContactIcon` 会同时替换
  `FriendInfo` 与 `ApplyInfo` 的 icon，并分别发出 `FriendIconChanged` / `ApplyIconChanged`，
  列表模型只刷新对应行；好友申请的 icon 由 ChatServer 的申请列表 SQL 直接关联 `user.icon` 得到。

个人资料页展示同步进度与「暂停/继续」按钮；同步失败不回滚本机头像，只显示错误码。

---

## 13.6 配置

```ini
[SelfServer]
Port = 9090

[Storage]
Path = storage

[Mysql]
Host = 127.0.0.1
XPort = 33060
User = root
Passwd = 123456
Schema = xigu

[Redis]
Host = 127.0.0.1
Port = 6380
Passwd = 123456

# 图片上传完成后通过 gRPC 通知 ChatServer 回写消息状态
[ChatServer]
Host = 127.0.0.1
Port = 50055
```

`Storage/Path` 相对服务工作目录；Docker 部署把它挂到 `resource-data` 卷
（`/var/lib/xiguchat/storage`），重建容器不会丢头像。

`StatusServer/config.ini` 的 `[ResourceServer]` 决定下发给客户端的地址：

```ini
[ResourceServer]
Host = 127.0.0.1
Port = 9090
```

`GetChatServerRsp` 的 `res_host` / `res_port` 随登录响应返回，GateServer 写入
`/user_login` 的 JSON；客户端 `AuthService` 解析到 `ServerInfo::ResHost/ResPort`。
未配置时字段为空，客户端只保存本机头像，不影响聊天登录。

---

## 13.7 已知边界

- 只支持单资源服务器；没有服务发现、实例选择和跨节点副本。
- 进度只在 Redis 保留 1 小时，超时后再次续传会从第一片重新开始。
- 聊天图片窗口固定 8 片，不做动态拥塞控制，也没有超时重传；失败会结束本次上传并标记消息失败。
- 服务端的 `accepted_seq` 只在内存中，进程重启后回落到已落盘进度；在途窗口内的分片需要客户端重新发送。
- 下载没有独立的进度持久化，断线后由客户端从本地文件大小重新发起。
- 本机已有头像文件时优先使用本机版本，不比较服务端 `icon` 是否变化；多设备同步需要额外记录本地头像来源。
- 聊天图片已接入压缩图上传、进度显示、状态回写与历史缩略图；文件消息、接收方推送与点开大图尚未接入。
