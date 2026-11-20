# 03 · 通信协议

系统使用三种协议：

| 协议 | 参与方 | 用途 | 编码 |
| --- | --- | --- | --- |
| HTTP/1.1 | Client ↔ GateServer | 注册、登录、改密、验证码 | JSON |
| TCP 自定义帧 | Client ↔ ChatServer | 长连接消息、好友、群聊、通知 | 4 字节头 + JSON |
| TCP 自定义帧 | Client ↔ ResourceServer | 头像/图片/视频/文件分片 | 6 字节头 + 分片 |
| gRPC | 服务之间 | 验证码、状态路由、跨实例转发 | protobuf |

三条面向客户端的通道都支持 TLS：配置证书后 HTTP 变 HTTPS，两条 TCP 协议在加密流上跑，
**帧格式、消息 ID 与 JSON 结构不变**。服务间 gRPC 仍是明文，只走回环或容器内网。

---

## 3.1 消息 ID 编号规则

消息 ID 是 `uint16`，按职责分段，段内请求与响应相邻：

| 段 | 用途 | 范围 |
| --- | --- | --- |
| 1xxx | HTTP 网关操作与业务错误码 | 1001–1106 |
| 2xxx | 连接、认证、心跳、登出 | 2001–2007 |
| 3xxx | 用户搜索、好友、资料 | 3001–3017 |
| 4xxx | 会话、消息、已读、撤回 | 4001–4021 |
| 5xxx | 群聊资料、设置、成员 | 5001–5009 |
| 6xxx | 资源通道（独立 TCP 连接） | 6001–6008 |

**响应 ID 固定为请求 ID + 1**（`ResponseId()`，定义在 `server/common/include/Protocol.h`）。
通知类消息（`ID_NOTIFY_*`）没有响应，不参与这条约定。服务端 `MAX_REQUEST_ID = 7000`，
读取帧头时先挡掉明显越界的 ID。

服务端定义在 `services/ChatServer/include/const.h` 的 `MSG_IDS`，客户端定义在
`client/core/global.h` 的 `ReqId`；资源通道的 6xxx 在 `ResourceServer/include/const.h`。
**修改协议必须同步两端**，客户端的 `static_assert` 会在请求/响应不再相邻时直接编译失败。

---

## 3.2 ChatServer 帧格式

```
 0        1        2        3        4 ...
+--------+--------+--------+--------+------------------+
|     msg_id      |   msg_len      |      payload      |
|  (2 bytes, BE)  |  (2 bytes, BE) |  (msg_len bytes)  |
+--------+--------+--------+---------------------------+
|<---------- HEAD_TOTAL_LEN = 4 --->|
```

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `HEAD_TOTAL_LEN` | 4 | 头部总长度 |
| `HEAD_ID_LEN` | 2 | msg_id 长度 |
| `HEAD_DATA_LEN` | 2 | msg_len 长度 |
| `MAX_LENGTH` | 2048 | **payload** 上限；头部单独读取 |
| `MAX_RESPONSE_LEN` | 65531 | 2 字节长度字段能承载的最大响应体 |

服务端 `CSession::AsyncReadHead` 先读满 4 字节，转换网络序，检查
`0 < msg_id <= MAX_REQUEST_ID` 与 `0 < msg_len <= MAX_LENGTH`，再分配 `RecvNode` 读包体。
读满后投递到逻辑分片队列，由 `LogicSystem::DealMsg` 统一解析 JSON、校验身份、分发处理器。
响应超过 `MAX_RESPONSE_LEN` 时回 `1106` 错误包，不直接断开连接。

客户端 `TcpMgr` 用 `QDataStream::BigEndian` 组帧；接收侧累积到 `_buffer`，
按同样的 4 字节头循环拆帧，半包状态由 `_b_recv_pending` 维护。

---

## 3.3 ChatServer 消息表

| ID | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| 2001 | `MSG_CHAT_LOGIN` | C → S | 登录长连接 |
| 2002 | `MSG_CHAT_LOGIN_RSP` | S → C | 登录回包 |
| 2003 | `ID_HEART_BEAT_REQ` | C → S | 心跳 |
| 2004 | `ID_HEARTBEAT_RSP` | S → C | 心跳回包 |
| 2005 | `ID_LOGOUT_REQ` | C → S | 主动登出 |
| 2006 | `ID_LOGOUT_RSP` | S → C | 登出结果 |
| 2007 | `ID_NOTIFY_OFF_LINE_REQ` | S → C | 通知下线（reason 1 顶号 / 2 过期 / 3 下线） |
| 3001 | `ID_SEARCH_USER_REQ` | C → S | 按 uid 或用户名搜索 |
| 3002 | `ID_SEARCH_USER_RSP` | S → C | 搜索结果 |
| 3003 | `ID_ADD_FRIEND_REQ` | C → S | 好友申请 |
| 3004 | `ID_ADD_FRIEND_RSP` | S → C | 申请回执 |
| 3005 | `ID_NOTIFY_ADD_FRIEND_REQ` | S → C | 有人申请加你为好友 |
| 3006 | `ID_AUTH_FRIEND_REQ` | C → S | 同意好友申请 |
| 3007 | `ID_AUTH_FRIEND_RSP` | S → C | 同意回包 |
| 3008 | `ID_NOTIFY_AUTH_FRIEND_REQ` | S → C | 对方已通过你的申请 |
| 3009 | `ID_DELETE_FRIEND_REQ` | C → S | 删除好友（只删自己一侧） |
| 3010 | `ID_DELETE_FRIEND_RSP` | S → C | 删除回包 |
| 3011 | `ID_UPDATE_FRIEND_REMARK_REQ` | C → S | 改好友备注与标签 |
| 3012 | `ID_UPDATE_FRIEND_REMARK_RSP` | S → C | 备注修改回包 |
| 3013 | `ID_GET_PROFILE_REQ` | C → S | 拉好友最新资料 |
| 3014 | `ID_GET_PROFILE_RSP` | S → C | 最新资料回包 |
| 3015 | `ID_NOTIFY_PROFILE_CHANGED` | S → C | 好友资料变更推送 |
| 3016 | `ID_UPDATE_PROFILE_REQ` | C → S | 修改本人资料 |
| 3017 | `ID_UPDATE_PROFILE_RSP` | S → C | 本人资料回包 |
| 4001 | `ID_CREATE_PRIVATE_CHAT_REQ` | C → S | 创建或复用单聊 |
| 4002 | `ID_CREATE_PRIVATE_CHAT_RSP` | S → C | 返回 thread_id 与对方资料 |
| 4003 | `ID_LOAD_CHAT_THREAD_REQ` | C → S | 拉会话列表 |
| 4004 | `ID_LOAD_CHAT_THREAD_RSP` | S → C | 会话列表与 load_more |
| 4005 | `ID_LOAD_CHAT_MSG_REQ` | C → S | 拉历史消息 |
| 4006 | `ID_LOAD_CHAT_MSG_RSP` | S → C | 历史消息与 load_more |
| 4007 | `ID_TEXT_CHAT_MSG_REQ` | C → S | 文本消息 |
| 4008 | `ID_TEXT_CHAT_MSG_RSP` | S → C | 文本回执 |
| 4009 | `ID_NOTIFY_TEXT_CHAT_MSG_REQ` | S → C | 文本投递 |
| 4010 | `ID_IMG_CHAT_MSG_REQ` | C → S | 图片消息（先落库） |
| 4011 | `ID_IMG_CHAT_MSG_RSP` | S → C | 图片回执 |
| 4012 | `ID_NOTIFY_IMG_CHAT_MSG_REQ` | S → C | 图片已上传完成 |
| 4013 | `ID_RESOURCE_CHAT_MSG_REQ` | C → S | 视频/文件消息（先落库） |
| 4014 | `ID_RESOURCE_CHAT_MSG_RSP` | S → C | 视频/文件回执 |
| 4015 | `ID_NOTIFY_RESOURCE_CHAT_MSG_REQ` | S → C | 视频/文件已上传完成 |
| 4016 | `ID_MARK_READ_REQ` | C → S | 上报已读位置 |
| 4017 | `ID_MARK_READ_RSP` | S → C | 已读回包 |
| 4018 | `ID_NOTIFY_MSG_READ` | S → C | 对端已读 |
| 4019 | `ID_RECALL_MSG_REQ` | C → S | 撤回消息 |
| 4020 | `ID_RECALL_MSG_RSP` | S → C | 撤回回包 |
| 4021 | `ID_NOTIFY_MSG_RECALLED` | S → C | 对端撤回 |
| 5001 | `ID_CREATE_GROUP_REQ` | C → S | 建群 |
| 5002 | `ID_CREATE_GROUP_RSP` | S → C | 建群结果 |
| 5003 | `ID_GET_GROUP_INFO_REQ` | C → S | 拉群资料与成员 |
| 5004 | `ID_GET_GROUP_INFO_RSP` | S → C | 群资料与成员 |
| 5005 | `ID_UPDATE_GROUP_REQ` | C → S | 改群名/公告/群昵称/免打扰 |
| 5006 | `ID_UPDATE_GROUP_RSP` | S → C | 群设置回包 |
| 5007 | `ID_GROUP_MEMBER_OP_REQ` | C → S | 邀请/移除/设管理员/退群/解散 |
| 5008 | `ID_GROUP_MEMBER_OP_RSP` | S → C | 成员操作回包 |
| 5009 | `ID_NOTIFY_GROUP_CHANGED` | S → C | 群资料、成员、系统消息推送 |

### 3.3.1 登录与保活

**2001** `{uid:int, token:string}`

**2002**

```jsonc
{
  "error": 0,
  "uid": 1002, "name": "xigu", "email": "a@b.com",
  "nick": "xigu", "desc": "", "sex": 0, "icon": "",
  "apply_list": [
    {"name":"", "uid":0, "icon":"", "nick":"", "sex":0, "desc":"", "status":0,
     "id":1, "direction":0}          // 0 别人申请我，1 我申请别人
  ],
  "friend_list": [
    {"name":"", "uid":0, "icon":"", "nick":"", "sex":0, "desc":"", "back":"", "tags":[]}
  ],
  "thread_list": [
    {"thread_id":1, "thread_type":1, "time":0, "unread":0,
     "uid":0, "name":"", "nick":"", "icon":"", "sex":0},
    {"thread_id":2, "thread_type":2, "time":0, "unread":0,
     "group_name":"", "owner":0, "member_count":0, "my_role":0, "muted":false, "at_me":false}
  ],
  "thread_more": false
}
```

`thread_list` 首屏最多 50 条；`thread_more` 为真时客户端用最后一条的 `thread_id + time`
继续发 4003。单聊带 `uid/name/nick/icon/sex`，群聊带 `group_*` 与 `my_role`。

**2003 / 2004** 心跳请求与响应，包体都是空对象 `{}`；服务端不解析业务字段。

**2005** 空对象；**2006** `{error, revoked}`。`revoked` 表示租约与 token 是否已删除。

**2007** `{error, uid, reason}`，服务端推送；reason 1 同账号异地登录、2 登录过期、3 服务端下线。
客户端收到后不得自动重连，必须回到登录页或提示用户。

### 3.3.2 搜索与好友

**3001** `{uid:string}` —— 纯数字按 uid 查，否则按用户名查。

**3002** `{error, uid, name, nick, desc, sex, icon, relation}`。
搜索只返回公开资料，不下发邮箱。`relation`：

| 值 | 含义 |
| --- | --- |
| 0 | 无关系 |
| 1 | 互为好友 |
| 2 | 我已申请，待对方处理 |
| 3 | 对方申请我，待我处理 |
| 4 | 单向关系（曾被删除） |
| -1 | 搜索的是自己 |

**3003** `{uid:int, applyname:string, bakname:string, touid:int, tags:[string]}`

`applyname` 是申请留言（空时服务端补默认句），`bakname` 是备注，`tags` 是分组。
`friend_apply` 以 `(from_uid,to_uid)` 唯一，重复申请会覆盖留言/备注/标签并把 `status` 复位为 0。

**3004** `{error}`。**3005** `{error, applyuid, name, desc, icon, sex, nick}`。

**3006** `{fromuid:int, touid:int, back:string, tags:[string]}`

**3007** `{error, name, nick, icon, sex, uid, thread_id, welcome_msgs, back}`。
`welcome_msgs` 项为 `{msgid, content, message_id, thread_id, status, fromuid, touid, time}`。

**3008** `{error, fromuid, touid, name, nick, icon, sex, thread_id, welcome_msgs, back, tags}`。
`back/tags` 是申请方当初填的备注与分组，通过后回传，申请方不必重新登录才看到。

**3009** `{touid:int}`；**3010** `{error, uid}`（`uid` 原样带回，失败时客户端可恢复乐观删除的行）。

**3011** `{touid:int, remark:string, tags:[string]}`；**3012** `{error, touid, remark, tags}`。

**3013** `{touid:int}`；**3014** `{error, uid, name, nick, desc, sex, icon}`。
**3015** 推送同样的资料字段，客户端整体覆盖本地缓存。

**3016** `{nick:string, desc:string, sex:int}`，`sex` 只接受 0/1/2；
**3017** `{error, uid, nick, sex, desc}`。

### 3.3.3 单聊会话与消息

**4001** `{fromuid:int, touid:int}`；**4002** `{error, thread_id, uid, name, nick, icon, sex}`。

**4003** `{fromuid:int, thread_id:int64, last_time:int64}`。
`thread_id` 与 `last_time` 是上一页最后一条的复合游标；首屏都传 0。

**4004** `{error, thread_list:[...], load_more:bool}`，条目与 2002 的 `thread_list` 相同。

**4005** `{fromuid:int, thread_id:int64, message_id:int64}` —— 取比游标大的消息，首屏传 0。

**4006**

```jsonc
{
  "error": 0, "thread_id": 1, "thread_type": 1, "load_more": false,
  "text_array": [
    {"message_id":0, "thread_id":1, "fromuid":0, "touid":0, "msgid":"",
     "content":"", "msg_type":0, "status":1, "time":0,
     "read":false, "recalled":false, "mentions":""}
  ]
}
```

`msg_type`：0 文本、1 图片、2 视频、3 文件、4 群系统消息。
图片的 `content` 是资源文件名；视频/文件的 `content` 是
`{"name":原始文件名,"size":字节数,"resource":资源名,"thumb":首帧资源名}`。
`read` 只对自己发的消息有意义；`recalled=true` 时正文已清空，客户端只渲染一行提示。

**4007** `{fromuid, touid, thread_id, thread_type, mentions:[int], text_array:[{msgid, content}]}`

`thread_type` 1 单聊 / 2 群聊；群聊的 `touid` 固定 0，`thread_id` 必须大于 0。

**4008** `{error, fromuid, touid, thread_id, thread_type, text_array:[{msgid, content, message_id, status, time, mentions}]}`

`status`：1 已发送、2 发送失败。写库失败且双方已不是好友时额外带 `not_friend: true`；
群消息失败且群无效时带 `group_invalid: true`。

**4009** 与 4008 的 `text_array` 结构相同，接收方看到的 `status=3`（未读）。

**4010** `{fromuid, touid, thread_id, thread_type, md5, name, token, unique_id, mentions:[int]}`

**4011** `{error, fromuid, touid, thread_id, thread_type, md5, name, unique_id, message_id, status:0, time}`

**4012** `{error, message_id, thread_id, fromuid, touid, msgid, name, total_size, time}`。
接收方收到后先查本地缓存，没有就向资源服务器断点下载。

**4013** `{fromuid, touid, thread_id, thread_type, msg_type, md5, name, file_name, file_size, thumb, token, unique_id, mentions:[int]}`

`msg_type` 只接受 2 视频 / 3 文件；`thumb` 是视频首帧资源名，可为空。

**4014** `{error, fromuid, touid, thread_id, thread_type, msg_type, name, unique_id, message_id, status:0, time}`

**4015** `{error, message_id, thread_id, fromuid, touid, msgid, name, total_size, msg_type, content, time}`。
接收方只建卡片，资源本体等用户点击后下载。

**4016** `{thread_id:int64, message_id:int64}`；**4017** `{error, thread_id, message_id}`。
单聊会推进 `conversation_member.last_read_message_id` 并通知对端；群聊只推进自己的游标。

**4018** `{error, thread_id, message_id, uid}`，`uid` 是读取者。

**4019** `{message_id:int64}`；**4020** `{error, thread_id, message_id, thread_type}`。
**4021** `{error, thread_id, message_id, fromuid}`。撤回窗口 120 秒，超时返回 `1012`。

### 3.3.4 群聊

**5001** `{fromuid:int, name:string, members:[int]}`，一次最多 50 位好友。

**5002**

```jsonc
{
  "error": 0, "thread_id": 2, "thread_type": 2,
  "group": {"thread_id":2, "name":"", "notice":"", "owner":0,
            "member_count":0, "my_role":2, "muted":false, "my_nick":"", "created_at":0},
  "members": [{"uid":0, "name":"", "nick":"", "icon":"", "sex":0, "role":0, "group_nick":""}]
}
```

**5003** `{thread_id:int64}`；**5004** 与 5002 的 `group + members` 相同，另带 `thread_id`。

**5005** `{thread_id:int64, action:string, value:string}`
`action`：`name` 群名、`notice` 公告、`nickname` 我在本群昵称、`mute` 免打扰（`"1"`/`"0"`）。

**5006** `{error, thread_id, action, value}`；`value` 是生效后的值。

**5007** `{thread_id:int64, action:string, members:[int], target:int}`

| action | 额外字段 | 说明 |
| --- | --- | --- |
| `invite` | `members` | 只能邀请自己的好友，已在群里的跳过 |
| `remove` | `target` | 群主可移除任何人（群主除外），管理员只能移除普通成员 |
| `set_admin` / `unset_admin` | `target` | 只有群主可以操作 |
| `leave` | 无 | 群主不能直接退群，需先解散 |
| `dismiss` | 无 | 只有群主可以解散；软删，成员与历史保留 |

**5008** `{error, thread_id, action, member_count, added:[int], target, message}`。

**5009** `{error, thread_id, thread_type:2, event:string, ...}`。
`event`：`created` 新群、`info` 群名/公告变化、`members` 成员或角色变化、
`removed` 我被移出、`dismissed` 群解散、`message` 一条群系统消息。
`message` 事件带 `message` 对象，字段与 4006 的单条消息一致。

---

## 3.4 ResourceServer 帧格式

```
 0        1        2        3        4        5        6 ...
+--------+--------+--------+--------+--------+--------+------------------+
|     msg_id      |            body_len            |      body        |
|  (2 bytes, BE)  |         (4 bytes, BE)          | (body_len bytes) |
+--------+--------+--------+--------+--------+--------+------------------+

body = 4 字节 JSON 头长度(BE) + JSON 头 + 数据段（可为空）
```

| ID | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| 6001 | `ID_UPLOAD_FILE_REQ` | C → S | 通用文件分段上传 |
| 6002 | `ID_UPLOAD_FILE_RSP` | S → C | 上传回执 |
| 6003 | `ID_SYNC_FILE_REQ` | C → S | 续传前同步进度 |
| 6004 | `ID_SYNC_FILE_RSP` | S → C | 已落盘位置 |
| 6005 | `ID_DOWN_LOAD_FILE_REQ` | C → S | 断点下载 |
| 6006 | `ID_DOWN_LOAD_FILE_RSP` | S → C | 下载分片 |
| 6007 | `ID_UPLOAD_HEAD_ICON_REQ` | C → S | 头像分段上传 |
| 6008 | `ID_UPLOAD_HEAD_ICON_RSP` | S → C | 头像上传回执 |

上传请求头：`{md5, name, seq, trans_size, total_size, last, uid, token, message_id?, touid?, thread_id?, thread_type?, unique_id?}`。
`md5` 是 32 位十六进制，`name` 是落盘文件名，`seq` 从 1 开始，
`trans_size` 是本片结束后的偏移，`last=1` 必须与 `trans_size==total_size` 同时成立。
每片原始数据最多 64KB，单帧包体上限 128KB，单文件上限 4GB。

下载请求头：`{uid, token, owner_uid, name, seq}`；下载回包头带
`{error, name, seq, is_last, current_size, total_size}`，数据段是本片内容。
下载鉴权：本人、同会话成员，或该文件仍是对方当前头像。

---

## 3.5 HTTP 接口（GateServer）

所有接口都是 `POST`，请求与响应都是 JSON；成功时 `error=0`。

| 路径 | 请求 | 成功响应 |
| --- | --- | --- |
| `/healthz` | GET | `{status, redis, mysql, uptime_seconds}`，依赖异常时 HTTP 503 |
| `/get_verifycode` | `{email}` | `{error, email}` |
| `/user_register` | `{email,user,passwd,confirm,verifycode}` | `{error, uid, email, user}` |
| `/reset_pwd` | `{email,user,passwd,verifycode}` | `{error, email, user}` |
| `/user_login` | `{email,passwd}` | `{error, uid, email, token, host, port, res_host, res_port}` |

- `passwd` 是客户端 xor 变形后的字符串（`123456` → `745230`），不是明文口令。
- 邮箱在入口统一 trim + 小写；注册、登录、重置、验证码共用同一个 Redis 键。
- 服务端校验用户名 2–20 字符、邮箱格式与长度、口令长度 6–15，
  不依赖客户端校验。
- `/get_verifycode` 同一邮箱 60 秒冷却，每小时 5 次；同一 IP 每小时 20 次。
- `/user_login` 同一账号 60 秒 5 次失败、同一 IP 60 秒 20 次失败后返回 `1105`。

---

## 3.6 gRPC 接口

proto 单一源在 `server/proto/message.proto`。

| 服务 | 方法 | 用途 |
| --- | --- | --- |
| `VerifyService` | `GetVerifyCode` | 生成验证码并发送邮件 |
| `StatusService` | `GetChatServer` | 按负载选 ChatServer 并签发 token |
| | `AllocateSession` | 申请租约、分配 epoch、顶掉旧会话 |
| | `RenewSessions` | 批量续租并上报实例负载 |
| | `ReleaseSession` / `LogoutSession` | 释放租约 / 登出并吊销 token |
| | `LookupRoute` / `BatchLookupRoute` | 查询在线位置 |
| | `RegisterInstance` / `UnregisterInstance` / `DrainInstance` | 实例注册、下线、排水 |
| `ChatService` | `NotifyAddFriend` / `NotifyAuthFriend` | 跨实例好友通知 |
| | `NotifyTextChatMsg` | 跨实例文本投递 |
| | `KickSession` | 顶号时关闭旧连接 |
| | `NotifyImageUploaded` | 资源落盘后回写消息状态并推送 |
| | `BroadcastProfile` / `NotifyProfileChanged` | 资料变更分发 |
| | `NotifyChatEvent` | 已读/撤回跨实例通知 |
| | `NotifyGroupEvent` | 群事件按实例聚合投递 |

gRPC 通用约定：调用本身返回 `OK`，业务结果放在响应报文的 `error` 字段。

---

## 3.7 错误码

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `Success` | 成功 |
| 1001 | `Error_Json` | 请求不是 JSON 对象或字段类型不合法 |
| 1002 | `RPCFailed` | RPC、数据库或缓存执行失败 |
| 1003 | `VerifyExpired` | 验证码不存在或已过期 |
| 1004 | `VerifyCodeErr` | 验证码不正确 |
| 1005 | `UserExist` | 用户名或邮箱已注册 |
| 1006 | `PasswdErr` | 密码错误；注册场景表示两次输入不一致 |
| 1007 | `EmailNotMatch` | 重置密码时用户名与邮箱不匹配 |
| 1008 | `PasswdUpFailed` | 重置密码写库失败 |
| 1009 | `PasswdInvalid` | 登录邮箱或密码不正确 |
| 1010 | `TokenInvalid` | 令牌缺失、失效或与 uid 不匹配 |
| 1011 | `UidInvalid` | uid 不存在或当前连接无权操作 |
| 1012 | `RecallExpired` | 超过 120 秒撤回窗口 |
| 1013 | `RecallDenied` | 不是发送者或消息已撤回 |
| 1014 | `NotFriend` | 双方已不是好友 |
| 1015 | `GroupNotFound` | 群不存在、已解散或不在群里 |
| 1016 | `GroupDenied` | 当前角色没有操作权限 |
| 1017 | `GroupFull` | 群成员已达上限 |
| 1018 | `GroupMemberExists` | 被邀请的人已经在群里 |
| 1022 | `VerifyTooFrequent` | 验证码发送超过冷却或小时配额 |
| 1023 | `VerifySendFailed` | 验证码已生成但邮件投递失败 |
| 1101 | `ServiceUnavailable` | 依赖服务暂不可用，可稍后重试 |
| 1102 | `SessionFenced` | 会话已被更高 epoch 顶替 |
| 1103 | `SessionExpired` | 租约过期且未续租 |
| 1104 | `InstanceUnknown` | 实例未注册或注册已过期 |
| 1105 | `TooManyRequests` | 短时间请求次数超限 |
| 1106 | `ResponseTooLarge` | 响应超过单帧上限 |

资源通道另有 `1201–1210`：`FileNotExists`、`FileSaveRedisFailed`、
`FileWritePermissionFailed`、`FileDataInvalid`、`FileNameInvalid`、`FileTooLarge`、
`FileSeqInvalid`、`FileReadPermissionFailed`、`FileOffsetInvalid`、`FileQuotaExceeded`。
它们只出现在 ResourceServer 的回包里，与通用错误码分开编号。
