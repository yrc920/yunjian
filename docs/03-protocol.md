# 03 · 通信协议

系统使用三种协议，各自承担不同职责：

| 协议 | 参与方 | 用途 | 编码 |
| --- | --- | --- | --- |
| HTTP/1.1 | Client ↔ GateServer | 注册、登录、改密、获取验证码 | JSON（`text/json`） |
| TCP 自定义帧 | Client ↔ ChatServer | 长连接实时消息与通知 | 4 字节头 + JSON 体 |
| TCP 自定义帧 | Client ↔ ResourceServer | 头像/文件分段传输 | 6 字节头 + JSON 体 |
| gRPC | 服务之间 | 验证码、状态路由、跨实例消息转发 | protobuf（`server/proto/message.proto`） |

---

## 3.1 TCP 私有协议（Client ↔ ChatServer）

### 帧格式

```
 0        1        2        3        4 ...
+--------+--------+--------+--------+------------------+
|     msg_id      |   msg_len      |      payload      |
|  (2 bytes, BE)  |  (2 bytes, BE) |  (msg_len bytes)  |
+--------+--------+--------+---------------------------+
|<---------- HEAD_TOTAL_LEN = 4 --->|
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `msg_id` | `uint16`，**网络字节序（大端）** | 消息类型，见消息 ID 表 |
| `msg_len` | `uint16`，**网络字节序** | **仅 payload 长度，不含头部** |
| `payload` | UTF-8 JSON | 业务数据 |

约束（服务端 `const.h`）：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `HEAD_TOTAL_LEN` | 4 | 头部总长度 |
| `HEAD_ID_LEN` | 2 | msg_id 长度 |
| `HEAD_DATA_LEN` | 2 | msg_len 长度 |
| `MAX_LENGTH` | 2048 | 单帧（头 + 体）上限，同时也是接收缓冲区大小 |

服务端解析流程（`CSession::AsyncReadHead`）：

1. `asyncReadFull(4)` 保证读满头部（内部用 `async_read_some` 循环补齐，应对 TCP 拆包）；
2. `network_to_host_short` 转换两个字段；
3. `msg_id > MAX_LENGTH` 或 `msg_len > MAX_LENGTH` 视为非法帧，直接 `ClearSession`；
4. 分配 `RecvNode(msg_len, msg_id)`，继续 `AsyncReadBody(msg_len)`；
5. 读完 payload 后追加 `'\0'`（`RecvNode` 多分配 1 字节），投入逻辑队列，然后重新监听下一帧头部。

客户端（`TcpMgr`）：

- 发送：`QDataStream` 设为 `BigEndian`，写入 `quint16 id`、`quint16 len`，再 append JSON 字节；
- 接收：`readyRead` 累积到 `_buffer`，用 `_b_recv_pending` 处理半包，按同样的 4 字节头循环拆帧；
  每帧新建 `QDataStream`（读取位置不会随缓冲消费复位）、用 `remove(0, n)` 消费数据，
  并校验 `0 < len <= 32763`（服务端 `short` 发送上限）；断开时清空缓冲与半包状态。

> 两端对字节序的处理是对称的：客户端 `QDataStream::BigEndian`，服务端
> `boost::asio::detail::socket_ops::network_to_host_short`。在小端机器（x86/ARM 均默认小端）
> 上，如果任一侧漏掉字节序转换，解析出的 msg_id 会是错误值——这是排查"消息不响应"时首先要核对的点。

### 消息 ID 表

服务端定义在 `services/ChatServer/include/const.h` 的 `MSG_IDS`，
客户端定义在 `client/core/global.h` 的 `ReqId`。**两处必须保持一致。**

| ID | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| 1005 | `MSG_CHAT_LOGIN` | C → S | 登录长连接 |
| 1006 | `MSG_CHAT_LOGIN_RSP` | S → C | 登录回包（含用户资料与好友数据） |
| 1007 | `ID_SEARCH_USER_REQ` | C → S | 搜索用户 |
| 1008 | `ID_SEARCH_USER_RSP` | S → C | 搜索回包 |
| 1009 | `ID_ADD_FRIEND_REQ` | C → S | 发起好友申请 |
| 1010 | `ID_ADD_FRIEND_RSP` | S → C | 申请回执 |
| 1011 | `ID_NOTIFY_ADD_FRIEND_REQ` | S → C | 通知被申请人"有人申请加你为好友" |
| 1013 | `ID_AUTH_FRIEND_REQ` | C → S | 同意好友申请 |
| 1014 | `ID_AUTH_FRIEND_RSP` | S → C | 同意回包（返回对方资料） |
| 1015 | `ID_NOTIFY_AUTH_FRIEND_REQ` | S → C | 通知申请方"对方已同意" |
| 1017 | `ID_TEXT_CHAT_MSG_REQ` | C → S | 发送文本消息 |
| 1018 | `ID_TEXT_CHAT_MSG_RSP` | S → C | 发送方回执 |
| 1019 | `ID_NOTIFY_TEXT_CHAT_MSG_REQ` | S → C | 向接收方投递消息 |
| 1025 | `ID_CREATE_PRIVATE_CHAT_REQ` | C → S | 创建或复用单聊线程 |
| 1026 | `ID_CREATE_PRIVATE_CHAT_RSP` | S → C | 返回 `thread_id` 与对方资料 |
| 1027 | `ID_LOAD_CHAT_THREAD_REQ` | C → S | 按 `thread_id` 游标拉会话列表 |
| 1028 | `ID_LOAD_CHAT_THREAD_RSP` | S → C | 会话列表与 `load_more` |
| 1029 | `ID_LOAD_CHAT_MSG_REQ` | C → S | 按 `thread_id + message_id` 拉历史消息 |
| 1030 | `ID_LOAD_CHAT_MSG_RSP` | S → C | 历史消息与 `load_more` |
| 1031 | `ID_IMG_CHAT_MSG_REQ` | C → S | 发送图片消息（先落库，资源随后上传） |
| 1032 | `ID_IMG_CHAT_MSG_RSP` | S → C | 图片消息回执（含消息 ID 与线程 ID） |
| 1033 | `ID_NOTIFY_IMG_CHAT_MSG_REQ` | S → C | 图片已上传完成，接收方开始下载 |
| 1034 | `ID_RESOURCE_CHAT_MSG_REQ` | C → S | 发送视频/文件消息（先落库，资源随后上传） |
| 1035 | `ID_RESOURCE_CHAT_MSG_RSP` | S → C | 视频/文件消息回执（含消息 ID 与线程 ID） |
| 1036 | `ID_NOTIFY_RESOURCE_CHAT_MSG_REQ` | S → C | 视频/文件已上传完成，接收方显示下载卡片 |
| 1045 | `ID_UPDATE_PROFILE_REQ` | C → S | 修改本人昵称、性别、个性签名 |
| 1046 | `ID_UPDATE_PROFILE_RSP` | S → C | 修改资料回包（带最终落库字段） |
| 1047 | `ID_UPDATE_FRIEND_REMARK_REQ` | C → S | 修改自己给某个好友设的备注与标签 |
| 1048 | `ID_UPDATE_FRIEND_REMARK_RSP` | S → C | 备注与标签修改回包 |
| 1049 | `ID_GET_PROFILE_REQ` | C → S | 按 uid 拉取好友最新资料 |
| 1050 | `ID_GET_PROFILE_RSP` | S → C | 最新资料回包（昵称/签名/头像/性别） |
| 1051 | `ID_NOTIFY_PROFILE_CHANGED` | S → C | 好友资料变更推送（昵称/签名/头像/性别） |
| 1052 | `ID_MARK_READ_REQ` | C → S | 上报会话已读位置 |
| 1053 | `ID_MARK_READ_RSP` | S → C | 已读上报回包 |
| 1054 | `ID_NOTIFY_MSG_READ` | S → C | 对端已读通知，发送方把单勾改成双勾 |
| 1055 | `ID_RECALL_MSG_REQ` | C → S | 撤回自己发的消息 |
| 1056 | `ID_RECALL_MSG_RSP` | S → C | 撤回回包 |
| 1057 | `ID_NOTIFY_MSG_RECALLED` | S → C | 对端撤回通知 |
| 1058 | `ID_DELETE_FRIEND_REQ` | C → S | 删除好友（只删自己一行，不通知对方） |
| 1059 | `ID_DELETE_FRIEND_RSP` | S → C | 删除好友回包 |
| 1060 | `ID_CREATE_GROUP_REQ` | C → S | 创建群聊（群名 + 好友 uid） |
| 1061 | `ID_CREATE_GROUP_RSP` | S → C | 建群回包（thread_id、群资料、成员） |
| 1062 | `ID_GET_GROUP_INFO_REQ` | C → S | 拉群资料与成员 |
| 1063 | `ID_GET_GROUP_INFO_RSP` | S → C | 群资料、成员与我的群设置 |
| 1064 | `ID_UPDATE_GROUP_REQ` | C → S | 改群名/公告、群昵称、免打扰 |
| 1065 | `ID_UPDATE_GROUP_RSP` | S → C | 群设置修改回包 |
| 1066 | `ID_GROUP_MEMBER_OP_REQ` | C → S | 邀请、移除、设/撤管理员、退群、解散 |
| 1067 | `ID_GROUP_MEMBER_OP_RSP` | S → C | 成员操作回包 |
| 1068 | `ID_NOTIFY_GROUP_CHANGED` | S → C | 群资料/成员/被移出/解散/系统消息推送 |
| 1069 | `ID_LOGOUT_REQ` | C → S | 主动登出，释放租约并吊销令牌 |
| 1070 | `ID_LOGOUT_RSP` | S → C | 登出结果，`revoked` 表示令牌是否已删除 |

> 1012 与 1016 为历史预留空位，当前未使用。1045–1048 与资源服务器的 1037–1044 不冲突：
> 后四者走 ResourceServer 的另一条连接，这里只用于 ChatServer 长连接。

### 载荷定义

服务端 JSON 由 jsoncpp 的 `toStyledString()` 生成（带缩进换行），客户端用 `QJsonDocument::fromJson` 解析，
两种格式兼容。以下列出各消息的字段。

**1005 登录请求** `{uid:int, token:string}`

**1006 登录回包**

```jsonc
{
  "error": 0,                       // 非 0 时其余字段可能缺失
  "uid": 1002, "name": "xigu", "email": "a@b.com",
  "nick": "xigu", "desc": "", "sex": 0, "icon": ":/res/head_1.jpg",
  "apply_list": [                   // 申请记录，direction 区分方向
    {"name":"", "uid":0, "icon":"", "nick":"", "sex":0, "desc":"", "status":0,
     "direction":0}                  // 0 别人申请我，1 我申请别人
  ],
  "friend_list": [                  // 好友列表
    {"name":"", "uid":0, "icon":"", "nick":"", "sex":0, "desc":"", "back":"", "tags":[]}
    // back 是本人给对方设的备注，空串表示没设，客户端按备注 > 昵称 > 用户名显示
    // tags 是本人给对方打的分组，只影响自己这一侧的联系人筛选
  ],
  "thread_list": [                  // 已建立的单聊会话
    {"thread_id":0, "uid":0, "name":"", "nick":"", "icon":"", "sex":0,
     "time":0, "unread":0}
    // time 是会话最后活动时间（UNIX 秒），unread 是发给自己的未读条数；
    // 客户端登录首屏直接用它恢复红点与排序，不能等到 1028 才有这两个字段
  ],
  "thread_more": false              // 会话是否还有下一页
}
```

> 登录与搜索只返回用户资料，不返回密码或密码哈希。除登录请求外，TCP 业务必须绑定已认证会话，操作者 uid/fromuid 必须与会话一致。

**1007 搜索请求** `{uid:string}` —— 传**字符串**，`isPureDigit` 判定：纯数字按 uid 查，否则按用户名查。

**1008 搜索回包** `{error, uid, name, email, nick, desc, sex, icon, relation}`
（按用户名查询时不含 `icon`）

`relation` 由服务端按库计算，客户端不再用本地缓存判断能不能加：

| 值 | 含义 | 客户端表现 |
| --- | --- | --- |
| 0 | 无任何关系 | 添加到通讯录 |
| 1 | 两行好友关系都在 | 发消息 |
| 2 | 我已申请，等待对方 | 已发送 |
| 3 | 对方申请我，待我处理 | 可添加，并提示去「新的朋友」 |
| 4 | 单向关系（曾被删除） | 可添加，提示"你们曾是好友" |
| -1 | 搜索的是自己 | 不显示添加入口 |

**1009 好友申请** `{uid:int, applyname:string, bakname:string, touid:int, tags:[string]}`

申请记录以 `(from_uid, to_uid)` 为唯一键，重复申请会更新留言、备注与标签，并把 `status`
复位成 0（待处理）——单向删除之后重新申请，对方才能在「新的朋友」里看到这条新申请。

`applyname` 是申请留言（为空时服务端用默认句），`bakname` 是备注名，`tags` 是给对方打的分组。
`bakname` 留空表示不设备注；`tags` 留空或省略表示不分组，最多 10 个、单个不超过 12 字符。
两者都存进 `friend_apply`，同意后复制到申请方那行 `friend`，之后可在好友资料页修改。

**1010 好友申请回执** `{error:int}` —— 非 0 表示申请没有写入，客户端保留输入并提示重试。

**1011 好友申请通知** `{error, applyuid, name, desc, icon, sex, nick}`

`name` 是申请人资料名，`desc` 是申请留言。

**1013 同意好友申请** `{fromuid:int, touid:int, back:string, tags:[string]}`（`back` 为备注名，同意方不填欢迎语）

`tags` 是同意方直接给对方选的分组，写进自己那一行 `friend`，通过后立即生效，
不必再去好友资料页补一次。对方在申请里填的备注与标签仍会复制到申请方那一行。

**1014 同意回包** `{error, name, nick, icon, sex, uid, thread_id, welcome_msgs, back}`

`back` 是同意方自己填的备注；`welcome_msgs` 项为 `{msgid, content, message_id, thread_id, status, fromuid, touid}`；
同意方视角：申请方留言（status=3）、同意方默认句（status=1）。

客户端把 1014 与 1015 分开处理：**1014 是"我同意了别人"**，界面直接进入会话，
默认招呼不算未读（人正看着）；**1015 是"别人通过了我"**，只把会话与红点放进列表，不强行切页。
两个回包走同一套装配（欢迎消息、会话、未读），1015 必须由会话层统一处理，
否则通过语不会计未读、单聊顶部的关系提示也不会消失。

**申请列表顺序**：`friend_apply.id` 倒序下发，客户端收到新申请时插到列表最前面，
和会话列表一致（最新在最上面）。

**1015 同意通知** `{error, fromuid, touid, name, nick, icon, sex, thread_id, welcome_msgs, back, tags}`

`back` 与 `tags` 是申请方当初填的备注与分组，通过后直接回传，申请方不必重新登录才看到；
`welcome_msgs` 项同上；申请方视角：自己的留言（status=1）、同意方默认句（status=3）。
那条默认句对申请方就是一条普通未读消息，红点用来提示"对方已经通过了"。

**1017 文本消息** `{fromuid:int, touid:int, thread_id:int64, text_array:[{content:string, msgid:string}]}`

`thread_id` 可为 0；服务端会按两人关系找到或创建线程。

**1018 文本消息回执** `{error, fromuid, touid, thread_id, text_array:[{content, msgid, message_id, status, time}]}`

`status`：1=已发送，2=发送失败。`time` 是服务端落库时间（UNIX 秒）。
失败时除了 `error`，若原因正是"双方已不是好友"，会额外带 `not_friend: true`，
客户端据此提示「对方已不是你的好友，消息未发送」，而不是笼统的发送失败。

**1019 文本消息投递** `{error, fromuid, touid, thread_id, text_array:[{content, msgid, message_id, status, time}]}`

接收方收到的消息 `status=3`（未读）。`time` 与发送方回执中的值相同，跨实例转发也原样带走。

**1025 创建单聊** `{fromuid:int, touid:int}`

**1026 创建单聊回包** `{error, thread_id, uid, name, nick, icon, sex}`

**1027 拉会话列表** `{fromuid:int, thread_id:int64}` —— `thread_id` 是上一页最后一条，首屏传 0。

**1028 会话列表回包** `{error, thread_list:[{thread_id, uid, name, nick, icon, sex, time, unread}], load_more:bool}`

`time` 是会话最后活动时间（UNIX 秒，来自 `chat_thread.updated_at`）。客户端登录首屏按它排序，
避免历史消息还没拉回来时列表先按加好友顺序显示、随后又跳一次；历史拉回后改用最后一条消息时间。
`unread` 是该会话里发给自己的未读条数，登录时一起下发——未读数因此能跨登录保留，
不必依赖客户端内存。

**1029 拉历史消息** `{fromuid:int, thread_id:int64, message_id:int64}` —— 取 `message_id` 之后的消息，首屏传 0。

**1030 历史消息回包** `{error, thread_id, text_array:[{message_id, thread_id, fromuid, touid, msgid, content, msg_type, status, time, read, recalled}], load_more:bool}`

`msg_type`：0=文本，1=图片（`content` 是资源文件名），2=视频，3=文件（2/3 的 `content` 是元数据 JSON）。
`time` 是 `chat_message.created_at` 的 UNIX 秒值，客户端按本地时区格式化成时间条，并据此排序会话。
`read` 表示对方是否已读（只对自己发的消息有意义）；`recalled` 为真时 `content` 已清空，
客户端只渲染一行「撤回了一条消息」，不再显示气泡与资源。

**1031 图片消息** `{fromuid:int, touid:int, thread_id:int64, md5:string, name:string, token:string, unique_id:string}`

`md5` 为 32 位十六进制，`name` 是资源文件名（`<md5>.jpg`，带透明像素的图用 `<md5>.png`），
`unique_id` 是客户端消息 ID。上传的是输入区压缩后的图（长边 1600），不是气泡缩略图。
服务端只做格式校验与落库（`chat_message.msg_type=1`、`status=0` 表示资源未上传）。
资源完整落盘后，ResourceServer 通过 gRPC `NotifyImageUploaded` 通知**接收方所在的 ChatServer**，
由它更新状态并推送 1033；接收方离线或跨实例通知失败时，ResourceServer 直接回写数据库。

**1032 图片消息回执** `{error, fromuid, touid, thread_id, message_id, md5, name, unique_id, status}`

`status=0`；客户端据此回填消息 ID 与线程 ID，然后向 ResourceServer 发送 `1037` 上传资源。
1037 的每个分片确认都会回推进度，上传完成时消息状态改为已发送。

**1033 图片完成通知** `{error, message_id, thread_id, fromuid, touid, msgid, name, total_size, time}`

`msgid` 是发送方的客户端消息 ID，与历史同步一致，接收方据此去重。`time` 是该消息的落库时间，
接收方用它画时间条并参与会话排序。接收方先查本机压缩图，
命中则直接显示缩略图；否则把消息插入会话并向 ResourceServer 发送 `1041` 断点下载，
进度写回同一条消息，完成后生成缩略图。

**1034 视频/文件消息**
`{fromuid, touid, thread_id, msg_type:2|3, md5, name, file_name, file_size:int64, thumb, token, unique_id}`

`msg_type=2` 为视频、`3` 为文件；`name` 是资源文件名，优先使用上传者的原文件名
（去掉路径分隔符与系统保留字符，重名追加 `(2)` 后缀），`file_name` 保留完整原文件名用于展示，
`thumb` 是视频首帧的资源名（仍按内容 md5 命名，文件消息为空）。服务端把 `file_name`、`file_size`、
`name`、`thumb` 写进 `chat_message.content` 的 JSON，`msg_type` 与 `status=0` 单独落库；
原始文件名只用于展示，不参与资源路径拼接。

**1035 视频/文件消息回执** `{error, fromuid, touid, msg_type, name, unique_id, thread_id, message_id, status}`

`status=0`；客户端回填消息 ID 与线程 ID 后开始 `1037` 上传。视频先上传首帧缩略图，
再把视频本体分片上传，两者都完成后才由资源服务器通知接收方。

**1036 视频/文件完成通知**
`{error, message_id, thread_id, fromuid, touid, msgid, name, total_size, msg_type, content, time}`

`content` 与历史同步里的一致，接收方据此渲染文件名、大小与首帧；`name` 就是原文件名，
下载端直接按这个名字落盘。`time` 是消息落库时间，用于时间条与会话排序。
收到 1036 时**不自动下载**
资源本体：视频与文件都先显示卡片和下载按钮，由用户点击后再向 ResourceServer 发 `1041`；
视频首帧缩略图较小，会在后台自动下载用于预览。

**1045 修改本人资料** `{nick:string, sex:int, desc:string}`

请求体不带 uid：操作者身份只取会话绑定的 uid，客户端无法借此修改别人的资料。
`nick` 与 `desc` 允许为空（昵称为空时界面回落到用户名），长度按 UTF-8 字符数限制：
昵称 20 字符、签名 60 字符；`sex` 只接受 0 未知 / 1 男 / 2 女。

**1046 修改资料回包** `{error, uid, nick, sex, desc}`

成功后服务端会同步刷新 `ubaseinfo_<uid>` 与 `nameinfo_<name>` 两份缓存，
回包字段就是最终落库值，客户端直接用它刷新本地资料。错误码：1001 字段不合法、1011 会话无效、1002 数据库失败。

**1047 修改好友备注与标签** `{touid:int, remark:string, tags:[string]}`

只写自己这一侧的 `friend.back` 与 `friend.tags`，对方看到的仍是对方自己的备注与分组。
`remark` 为空表示清除备注，长度上限与昵称相同（20 字符）；`tags` 是整组替换，
空数组表示清空分组，最多 10 个、单个不超过 12 字符。不是好友时返回 1011，数据库失败返回 1002。

**1048 备注与标签修改回包** `{error, touid, remark, tags:[string]}`

两个字段都是服务端确认后的值，客户端据此更新好友列表、聊天标题、资料页与筛选条。

**1049 拉取好友最新资料** `{touid:int}`

只读请求，不要求对方在线或已是好友（搜索结果也会用到）。服务端优先读
`ubaseinfo_<uid>` 缓存，未命中再查库并回填。用于聊天标题与资料页刷新昵称和个性签名。

**1050 最新资料回包** `{error, uid, name, nick, desc, sex, icon}`

不返回邮箱与密码；`error=1011` 表示 uid 无效。客户端在打开会话与资料页时各拉一次，
失败时静默保留本地缓存，不打断当前页面。

**1051 好友资料变更推送** `{error, uid, name, nick, desc, sex, icon}`

好友改了昵称、签名或头像后由服务端主动下发，客户端整体覆盖本地缓存：
文字字段立即刷新列表与聊天标题，`icon` 与本地文件名不同时再走一次头像下载。
推送只发给在线好友；离线好友不做补偿，下次登录从 1006 回包拿到最新资料。
自己的资料仍以 1046 回包为准，不会收到自己的推送。

**1052 上报已读** `{thread_id:int64, message_id:int64}`

客户端打开会话、或在会话页收到新消息时上报"已读到哪一条"。服务端只把该会话里
`recv_id = 自己` 且 `message_id <= 上报值` 的消息标为已读；同一条不会重复通知对方。

**1053 已读上报回包** `{error, thread_id, message_id}`

只是确认，客户端不需要额外动作。参数非法返回 1001，会话不属于自己返回 1002。

**1054 对端已读通知** `{thread_id, uid, message_id}`

`uid` 是读取者。发送方收到后，把自己发的、`message_id` 不超过这个位置的消息标成已读（双勾）。
离线时不推，下次拉历史由 `read` 字段还原。

**1055 撤回消息** `{message_id:int64}`

只带消息号：归属与时间窗由服务端按库里的记录判断。只允许发送者本人、发送后 2 分钟内、未撤回的消息。

**1056 撤回回包** `{error, thread_id, message_id}`

成功返回 0；超过 2 分钟返回 `1012`，不是本人或已撤回返回 `1013`，消息不存在同样按 `1013` 处理。

**1057 对端撤回通知** `{thread_id, message_id, fromuid}`

接收方把这条消息标为已撤回：正文清空，不再显示气泡与附件，只留一行「「昵称」撤回了一条消息」。

**1058 删除好友** `{touid:int}`

只带对方 uid。服务端**只删自己这一行** `friend` 关系（以及自己发出的申请记录），
不推通知；对方那一行仍在，所以他的好友列表照旧，也能继续看到会话。
`private_chat` 与 `chat_message` 始终保留。

发消息要求两行都在（`EnsureChatThread` 校验 COUNT=2），因此**任何一方删除都会让双方发不出去**；
失败回包带 `not_friend:true`，客户端据此显示关系提示与「发送朋友验证」入口。

**1059 删除好友回包** `{error, uid}`

成功返回 0 与对方 uid；关系不存在或参数非法返回非 0。成功后客户端从联系人列表移除，
会话列表保留该会话（历史仍可查看）。删除只影响自己这侧的联系人；双方都可以重新提交好友申请，
通过后 `AddFriend` 用 `INSERT IGNORE` 补回缺的那一行，历史会话继续沿用。

### 群聊（1060–1068）

群聊与单聊共用 `thread_id` 与消息表：`chat_thread.thread_type=2`，群消息 `recv_id=0`。
1017/1019、1031–1036 都带 `thread_type`；群聊里 `touid=0`，目标由 `thread_id` 决定。

**1060 建群** `{fromuid, name, members:[uid...]}`

创建者成为群主，成员只能是创建者的好友；一次最多 50 人，群上限 200。
`name` 为空时服务端用「群聊」。

**1061 建群回包** `{error, thread_id, thread_type, group, members}`

`group` 字段：`thread_id/name/notice/owner/member_count/my_role/muted/my_nick/created_at`；
`members` 每项：`uid/name/nick/icon/sex/role/group_nick`。
成功后会给其他成员推 1068 `created`，并写一条建群系统消息。

**1062 拉群资料** `{fromuid, thread_id}` → **1063** `{error, thread_id, group, members}`

只允许群成员读取；`group` 额外带 `my_role`、`muted`、`my_nick`。

**1064 修改群设置** `{fromuid, thread_id, action, value}`

`action` 取 `name` / `notice` / `nickname` / `mute`；前两项需管理员及以上，
`nickname` 改我在本群的昵称（≤16 字），`mute` 的 `value` 为 `"1"`/`"0"`。
**1065** 回包 `{error, thread_id, action, value}`；改名与改公告会写系统消息并推 1068 `info`。

**1066 成员操作** `{fromuid, thread_id, action, ...}`

- `invite`：`members:[uid...]`，只能邀请自己的好友，已在群里的跳过；
- `remove` / `set_admin` / `unset_admin`：`target:uid`；
- `leave`：群主不能直接退群；
- `dismiss`：只有群主可以解散，删 `chat_thread` 时消息与成员级联清理。

**1067 回包** `{error, thread_id, action, member_count?, added?, target?, message?}`。

**1068 群变更推送** `{error, thread_id, thread_type:2, event, ...}`

`event` 取值：

- `created`：带 `group` 与 `members`，被拉进群的人据此建会话；
- `info`：带 `name` 或 `notice`；
- `members`：带 `member_count`，可能带 `target` 与 `role`；
- `removed` / `dismissed`：我被移出或群解散，客户端删除会话；
- `message`：带一条系统消息 `message`（字段同历史消息）。

群消息未读按 `group_chat_member.last_read_message_id` 推进，`at_me` 由未读消息的
`mentions` 是否含当前 uid 判断；群聊不做逐条已读。

### 登出（1069）

**1069 登出**：请求体为空对象 `{}`，身份只认当前会话，不接受客户端传入的 uid。

服务端按 `session_id + epoch` 调 `StatusServer::LogoutSession`，只有当前租约的持有者
才能删除 `sess_<uid>` 与 `utoken_<uid>`；被顶掉的旧连接即使发出登出，也不会影响新会话。
**1070 回包** `{error, revoked}`：`revoked=true` 表示租约与令牌都已删除。

状态服务不可用时本地照常关闭连接，令牌由 7 天 TTL 兜底；普通掉线、被踢与登录超时
都不会触发登出，只有用户明确退出才发送这条消息。

---

### 资源服务器私有协议（Client ↔ ResourceServer）

资源服务器使用同一套「ID + 长度 + JSON」思路，但长度字段扩为 4 字节，容纳文件分片。
消息 ID 仍沿用 1000 段（1037–1044），只是走另一条 TCP 连接，与聊天协议不会在同一连接上冲突：

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
| `MAX_FILE_LEN` | 2048 | 每个分片的原始字节数，两端一致 |
| `MAX_MESSAGE_LEN` | 16384 | 单帧包体上限 |
| `MAX_FILE_SIZE` | 4GB | 单文件上限 |

| ID | 名称 | 方向 | 说明 |
| --- | --- | --- | --- |
| 1037 / 1038 | `ID_UPLOAD_FILE_REQ/RSP` | C ↔ S | 通用文件分段上传 |
| 1039 / 1040 | `ID_SYNC_FILE_REQ/RSP` | C ↔ S | 续传前同步服务器进度 |
| 1041 / 1042 | `ID_DOWN_LOAD_FILE_REQ/RSP` | C ↔ S | 断点下载 |
| 1043 / 1044 | `ID_UPLOAD_HEAD_ICON_REQ/RSP` | C ↔ S | 头像分段上传 |

上传首个分片必须携带 `uid`、`token`，服务端比对 Redis `utoken_<uid>` 后把 uid 绑定到连接；
后续分片只校验连接身份。字段、校验规则与断点续传流程见
[13-ResourceServer](13-resource-server.md)。

聊天资源（图片、视频、文件）的 `1037` 请求额外带 `message_id`、`touid`、`thread_id`、
`unique_id`，供资源服务器最后一片写完后回写消息状态。客户端使用拥塞窗口，聊天资源最多 8 片
在途（头像仍逐片确认），服务端按“已接收序号”推进，不再要求每一片都等上一片的落盘和回包。
暂停时客户端停止补发分片，已在途的分片仍会落盘；继续时先发 `1039` 取服务器已写到的序号，
再按该序号续传，因此暂停期间的迟到回包不会造成重复或空洞。下载暂停只是不再请求下一片，
继续时从内存里的 `seq` 接着要，不需要重新协商进度。

---

## 3.2 HTTP 接口（Client ↔ GateServer）

Base URL 由客户端 `config.ini` 的 `[GateServer] host/port` 拼成 `http://<host>:<port>`。
默认 `http://127.0.0.1:8080`。所有响应 `Content-Type: text/json`，`Server: GateServer`，
连接为 `keep_alive(false)`（每请求一连接）。

路由注册在 `GateServer/src/LogicSystem.cpp`，未匹配的路径返回 404 `url not found`。

### `GET /get_test`

连通性测试，回显查询参数。用于验证 GET 参数解析（URL 解码由 `HttpConnection::PreParseGetParam` 完成）。

### `POST /get_verifycode`

请求：`{"email": "user@example.com"}`

响应：`{"error": int, "email": string}`

流程：先按邮箱 60 秒冷却与邮箱/IP 小时配额限流（超限 1022），再 gRPC 调用
`VerifyService.GetVerifyCode`，验证服务的错误码直接透传（0/1/2/3，见错误码表）。

### `POST /user_register`

请求：

```json
{"email": "...", "user": "...", "passwd": "...", "confirm": "...", "verifycode": "..."}
```

校验顺序：

1. JSON 解析失败或缺字段 → `1001`；
2. `passwd != confirm` → `1006`；
3. Redis `GET code_<email>` 不存在 → `1003`（验证码过期）；
4. 与请求中的 `verifycode` 不等 → `1004`；
5. `MysqlMgr::RegUser()` 返回 `0` 或 `-1` → `1005`（用户名或邮箱已存在）。

成功响应：`{"error":0, "uid":int, "email":string, "user":string, "verifycode":string}`

### `POST /reset_pwd`

请求：`{"email": "...", "user": "...", "passwd": "...", "verifycode": "..."}`

校验顺序：JSON → 验证码存在性（1003）→ 验证码比对（1004）→ `CheckEmail(name, email)`（1007）
→ `UpdatePwd(name, pwd)`（1008，失败时）。

> 新密码在 DAO 层做 Argon2id 哈希后落库；网关只接收客户端 `xorString()` 变形后的字符串，
> 不做还原，因此哈希的原文是变形值而不是用户输入的原密码。

成功响应：`{"error":0, "email":string, "user":string, "verifycode":string}`

### `POST /user_login`

请求：`{"email": "...", "passwd": "..."}`

校验顺序：JSON（1001）→ `CheckPwd`（1009 邮箱或密码不匹配）→ `StatusGrpcClient::GetChatServer`（1002 RPC 失败）。

成功响应：

```json
{"error":0, "email":"...", "uid":1002, "token":"<uuid>", "host":"127.0.0.1", "port":"8090"}
```

客户端随后用 `host:port` 建立 TCP 连接，并用 `uid + token` 发送 1005 完成长连接登录。

---

## 3.3 gRPC 接口（`server/proto/message.proto`）

`package message;`，三个 service：

### `VerifyService`（VerifyServer，50051）

```proto
rpc GetVerifyCode (GetVerifyReq) returns (GetVerifyRsp);
// GetVerifyReq { string email = 1; }
// GetVerifyRsp { int32 error = 1; string email = 2; string code = 3; }
```

调用方：GateServer（`VerifyGrpcClient`）。

### `StatusService`（StatusServer，50052）

```proto
rpc GetChatServer      (GetChatServerReq)      returns (GetChatServerRsp);
rpc AllocateSession    (AllocateSessionReq)    returns (AllocateSessionRsp);
rpc RenewSessions      (RenewSessionsReq)      returns (RenewSessionsRsp);
rpc ReleaseSession     (ReleaseSessionReq)     returns (ReleaseSessionRsp);
rpc LogoutSession      (LogoutSessionReq)      returns (LogoutSessionRsp);
rpc LookupRoute        (LookupRouteReq)        returns (LookupRouteRsp);
rpc BatchLookupRoute   (BatchLookupRouteReq)   returns (BatchLookupRouteRsp);
rpc RegisterInstance   (RegisterInstanceReq)   returns (RegisterInstanceRsp);
rpc UnregisterInstance (UnregisterInstanceReq) returns (UnregisterInstanceRsp);
rpc DrainInstance      (DrainInstanceReq)      returns (DrainInstanceRsp);
```

| 消息 | 字段 |
| --- | --- |
| `GetChatServerReq` / `Rsp` | `uid` / `error`, `host`, `port`, `token`, `res_host`, `res_port` |
| `AllocateSessionReq` | `uid`, `token`, `session_id`, `instance`, `device_class` |
| `AllocateSessionRsp` | `error`, `epoch`, `lease_seconds`, `kicked_instance`, `kicked_uid` |
| `SessionLeaseRef` | `uid`, `session_id`, `epoch` |
| `RenewSessionsReq` / `Rsp` | `instance`, `items`, `conn_count`, `session_count` / `error`, `items`, `draining` |
| `SessionLeaseResult` | `status`（0 正常 / 1 重新授予 / 2 被顶替 / 3 错误）, `uid`, `epoch`, `session_id` |
| `ReleaseSessionReq` / `Rsp` | `uid`, `session_id`, `epoch` / `error`, `released` |
| `LogoutSessionReq` / `Rsp` | `uid`, `session_id`, `epoch` / `error`, `revoked` |
| `LookupRouteReq` / `Rsp` | `uid` / `error`, `online`, `instance`, `epoch` |
| `BatchLookupRouteReq` / `Rsp` | `uids` / `error`, `entries` |
| `RegisterInstanceReq` / `Rsp` | `name`, `host`, `rpc_host`, `client_port`, `rpc_port`, `capacity` / `error`, `heartbeat_seconds`, `draining` |
| `DrainInstanceReq` / `Rsp` | `name`, `grace_seconds` / `error` |

`GetChatServer` 的调用方是 GateServer；其余由 ChatServer 与 ResourceServer 调用。
所有响应都是 gRPC 成功、业务结果看 `error`：1101 状态服务不可用、1102 会话被顶替、
1103 租约过期、1104 实例未注册、1105 登录过于频繁。

### `ChatService`（ChatServer 实例，50055 / 50056 …）

```proto
rpc NotifyAddFriend    (AddFriendReq)    returns (AddFriendRsp);
rpc NotifyAuthFriend   (AuthFriendReq)   returns (AuthFriendRsp);
rpc NotifyTextChatMsg  (TextChatMsgReq)  returns (TextChatMsgRsp);
rpc KickSession        (KickSessionReq)  returns (KickSessionRsp);
rpc NotifyImageUploaded(NotifyImageUploadedReq) returns (NotifyImageUploadedRsp);
rpc BroadcastProfile   (BroadcastProfileReq)    returns (BroadcastProfileRsp);
rpc NotifyProfileChanged(ProfileChangedReq)     returns (ProfileChangedRsp);
rpc NotifyChatEvent    (ChatEventReq)    returns (ChatEventRsp);
rpc NotifyGroupEvent   (GroupEventReq)   returns (GroupEventRsp);
```

| 消息 | 字段 |
| --- | --- |
| `AddFriendReq` | `applyuid`, `name`, `desc`, `icon`, `nick`, `sex`, `touid` |
| `AddFriendRsp` | `error`, `applyuid`, `touid` |
| `AuthFriendReq` | `fromuid`, `touid` |
| `AuthFriendRsp` | `error`, `fromuid`, `touid` |
| `TextChatMsgReq` | `fromuid`, `touid`, `repeated TextChatData textmsgs` |
| `TextChatData` | `msgid`, `msgcontent` |
| `TextChatMsgRsp` | `error`, `fromuid`, `touid`, `repeated TextChatData textmsgs` |
| `KickSessionReq` / `Rsp` | `uid`, `epoch`, `reason`, `session_id` / `error`, `closed` |
| `NotifyImageUploadedReq` | `message_id`, `sender`, `receiver`, `thread_id`, `name`, `client_msg_id`, `total_size` |
| `NotifyImageUploadedRsp` | `error`, `message_id` |

实际使用：`NotifyAddFriend`、`NotifyAuthFriend`、`NotifyTextChatMsg` 用于跨实例转发，
`KickSession` 由 StatusServer 在异地登录时调用（按旧连接的 `session_id` 精确定位）；
`NotifyImageUploaded` 由 **ResourceServer** 在聊天图片全部落盘后调用。
ResourceServer 先向 StatusServer 查询接收方所在实例，
ChatServer 收到后把 `chat_message.status` 从 0（未上传）改成 1（已上传），
如果接收方在本实例在线，再推送 1033 通知其下载。

所有 RPC 使用 `grpc::InsecureServerCredentials()` / `InsecureChannelCredentials()`，
即**服务间通信未加密**，适用于受信任的内网部署。

---

## 3.4 错误码总表

### 服务端错误码（`GateServer/include/const.h`、`ChatServer/include/const.h`、`ResourceServer/include/const.h`）

| 值 | 名称 | 含义 | 出现位置 |
| --- | --- | --- | --- |
| 0 | `Success` | 成功 | 全部 |
| 1001 | `Error_Json` | JSON 解析失败或缺字段 | HTTP 接口 |
| 1002 | `RPCFailed` | gRPC 调用失败 | HTTP 接口、gRPC 响应 |
| 1003 | `VerifyExpired` | 验证码不存在或已过期 | 注册、改密 |
| 1004 | `VerifyCodeErr` | 验证码不匹配 | 注册、改密 |
| 1005 | `UserExist` | 用户名或邮箱已存在 | 注册 |
| 1006 | `PasswdErr` | 两次密码不一致 | 注册 |
| 1007 | `EmailNotMatch` | 用户名与邮箱不匹配 | 改密 |
| 1008 | `PasswdUpFailed` | 更新密码失败 | 改密 |
| 1009 | `PasswdInvalid` | 邮箱或密码错误 | 登录 |
| 1022 | `VerifyTooFrequent` | 验证码发送过于频繁 | 获取验证码 |
| 1010 | `TokenInvalid` | Token 不匹配 | 长连接登录、StatusServer::Login |
| 1011 | `UidInvalid` | uid 无效（Redis 无 token / 用户不存在） | 长连接登录、通知类消息 |
| 1012 | `FileNotExists` | 服务器没有这次传输的记录 | 资源续传同步 |
| 1013 | `FileSaveRedisFailed` | 续传进度写入 Redis 失败 | 资源上传 |
| 1014 | `FileWritePermissionFailed` | 目标文件无法创建或写入 | 资源上传 |
| 1015 | `FileDataInvalid` | 分片数据与声明偏移不一致 | 资源上传 |
| 1016 | `FileNameInvalid` | 文件名含路径分隔符或超长 | 资源上传/下载 |
| 1017 | `FileTooLarge` | 文件超过 4GB 上限 | 资源上传 |
| 1018 | `FileSeqInvalid` | 分片序号不连续 | 资源上传 |
| 1019 | `FileReadPermissionFailed` | 源文件无法打开或读取 | 资源下载 |
| 1020 | `FileOffsetInvalid` | 请求偏移已超出文件范围 | 资源下载 |

> `ChatServer/include/const.h` 在 1001–1009 的基础上追加了 1010/1011；
> `ResourceServer/include/const.h` 追加了 1012–1020；`GateServer` 为验证码限流加了 1022；
> StatusServer 复用同一份 1010/1011 定义。错误码与消息 ID 是两套独立编号。
> 各处 `ErrorCodes` 目前是**各自独立复制**的枚举，修改时需同步。

### 验证码服务错误码（`VerifyServer/include/const.h`，`VerifyCodes`）

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `Success` | 验证码已发送 |
| 1 | `RedisErr` | 写入 Redis 失败 |
| 2 | `Exception` | 处理过程抛出异常 |
| 3 | `SendMailErr` | 发送邮件失败 |

该错误码由 GateServer 原样写入 HTTP 响应的 `error` 字段，客户端可据此提示"邮件发送失败"。

### 客户端错误码（`client/core/global.h`，`ErrorCodes`）

| 值 | 名称 | 含义 |
| --- | --- | --- |
| 0 | `SUCCESS` | 成功 |
| 1 | `ERR_JSON` | JSON 解析失败 |
| 2 | `ERR_NETWORK` | 网络错误（HTTP 层） |

这是**客户端本地**的错误码空间，与服务端 1001+ 的错误码不是同一套，
客户端在展示时直接用服务端透传的数值。

### 服务端错误码到界面文案的映射（`client/core/service/authservice.cpp`）

界面不直接显示数字，由 `AuthService` 按流程翻译。同一码在不同流程含义不同的只有 1006：

| 码 | 登录 | 注册 | 重置密码 |
| --- | --- | --- | --- |
| 1001 | 请求数据不完整，请重试 | 同左 | 同左 |
| 1002 | 服务器繁忙，请稍后再试 | 同左 | 同左 |
| 1003 | — | 验证码已过期，请重新获取 | 同左 |
| 1004 | — | 验证码不正确 | 同左 |
| 1005 | — | 该邮箱或用户名已被注册 | — |
| 1006 | 密码不正确 | 两次输入的密码不一致 | — |
| 1007 | — | — | 用户名与邮箱不匹配 |
| 1008 | — | — | 密码更新失败，请稍后再试 |
| 1009 | 邮箱或密码不正确 | — | — |
| 其他 | 操作失败（错误码 N） | 同左 | 同左 |

> 登录失败统一提示「邮箱或密码不正确」，不区分账号是否存在，避免枚举账号。


---

## 3.5 协议扩展指南

新增一个客户端 ↔ ChatServer 的消息需要改动**四处**：

1. `server/services/ChatServer/include/const.h` 的 `MSG_IDS` 增加 ID；
2. `client/core/global.h` 的 `ReqId` 增加同名同值 ID；
3. 服务端 `LogicSystem::RegisterCallBacks()` 注册处理函数（处理函数在单一逻辑线程执行）；
4. 客户端 `TcpMgr::initHandlers()` 注册处理函数，并在 UI 层连接对应信号。

新增一个服务间 RPC 需要改动：

1. `server/proto/message.proto` 定义消息与 rpc（构建期自动生成代码，无需手工执行 protoc）；
2. 服务提供方实现 `ServiceImpl` 中的方法；
3. 调用方在 `*GrpcClient` 中增加方法，复用已有 Stub 连接池。
