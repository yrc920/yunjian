# 03 · 通信协议

## 3.1 号段与约定

| 号段 | 用途 | 载体 |
| --- | --- | --- |
| `1001–1004` | 客户端 HTTP 请求的内部关联 | HTTP |
| `2001–2007` | 连接、登录、心跳、登出 | TCP 长连接 |
| `3001–3017` | 用户搜索、好友、资料 | TCP 长连接 |
| `4001–4021` | 单聊会话、消息、已读、撤回 | TCP 长连接 |
| `5001–5009` | 群聊资料、成员与推送 | TCP 长连接 |
| `6001–6008` | 头像与聊天资源传输 | 独立资源 TCP |
| `0 / 1xxx / 11xx / 12xx` | 业务错误码 | 所有响应 |

三条硬约定：

1. **请求与响应相邻**：响应号固定为请求号 + 1，两端都有 `static_assert`。
2. **先解析再分发**：服务端只解析一次 JSON，handler 拿到的是 `Json::Value`。
3. **失败带 `error`**：所有响应第一个语义字段是 `error`，`0` 表示成功。

## 3.2 ChatServer 帧格式

| 字段 | 长度 | 说明 |
| --- | --- | --- |
| `msg_id` | 2 字节，大端 | 消息号 |
| `length` | 2 字节，大端 | JSON 体字节数 |
| `body` | `length` 字节 | 紧凑 JSON |

| 项 | 规则 |
| --- | --- |
| 字节序 | 网络序（大端） |
| 请求体上限 | 8 KB（`MAX_LENGTH`） |
| 响应体上限 | 65531 B；超出回 `1106` 而不是断开 |
| 编码 | JSON，`JsonToCompact()` 紧凑输出，不缩进 |
| 超大文本 | 客户端按 7000 字节分帧，每帧是一条独立消息 |

## 3.3 消息目录

| 请求 | 响应 | 名称 | 方向 |
| --- | --- | --- | --- |
| 2001 | 2002 | 聊天登录 | C → S |
| 2003 | 2004 | 心跳 | C → S |
| 2005 | 2006 | 登出 | C → S |
| — | 2007 | 被踢下线通知 | S → C |
| 3001 | 3002 | 搜索用户 | C → S |
| 3003 | 3004 | 申请加好友 | C → S |
| — | 3005 | 收到好友申请 | S → C |
| 3006 | 3007 | 同意好友申请 | C → S |
| — | 3008 | 对方同意了你的申请 | S → C |
| 3009 | 3010 | 删除好友 | C → S |
| 3011 | 3012 | 改备注与标签 | C → S |
| 3013 | 3014 | 拉取好友资料 | C → S |
| — | 3015 | 好友资料变更 | S → C |
| 3016 | 3017 | 修改本人资料 | C → S |
| 4001 | 4002 | 打开单聊 | C → S |
| 4003 | 4004 | 会话列表分页 | C → S |
| 4005 | 4006 | 历史消息分页 | C → S |
| 4007 | 4008 | 文本 / 合并转发 | C → S |
| — | 4009 | 新消息通知 | S → C |
| 4010 | 4011 | 图片消息占位 | C → S |
| — | 4012 | 图片已上传 | S → C |
| 4013 | 4014 | 视频 / 文件占位 | C → S |
| — | 4015 | 视频 / 文件已上传 | S → C |
| 4016 | 4017 | 上报已读 | C → S |
| — | 4018 | 对方已读 | S → C |
| 4019 | 4020 | 撤回消息 | C → S |
| — | 4021 | 对方撤回 | S → C |
| 5001 | 5002 | 创建群聊 | C → S |
| 5003 | 5004 | 拉群资料 | C → S |
| 5005 | 5006 | 改群设置 | C → S |
| 5007 | 5008 | 群成员操作 | C → S |
| — | 5009 | 群资料 / 成员变化 | S → C |

## 3.4 连接与登录

**2001 请求**

```json
{"uid": 1003, "token": "utoken 的值"}
```

**2002 响应**

| 字段 | 说明 |
| --- | --- |
| `error` | 0 成功；`1010` 令牌无效；`1011` uid 不存在；`1105` 登录过于频繁 |
| `uid / name / email / nick / desc / sex / icon` | 本人资料 |
| `thread_list` | 首屏最多 50 条会话 |
| `thread_more` | 是否还有更多会话 |
| `apply_list` | 收到与发出的好友申请，`direction` 0/1 区分 |
| `friend_list` | 好友资料与备注、标签 |

会话项字段：

```json
{"thread_id": 1, "thread_type": 1, "time": 1795148660, "unread": 2,
 "uid": 1002, "name": "霄宫", "nick": "烟花", "icon": "", "sex": 1,
 "group_name": "", "icon_owner": 0, "owner": 0, "member_count": 0,
 "my_role": 0, "muted": false, "at_me": false}
```

单聊填 `uid/name/nick/icon/sex`；群聊填 `group_name/icon/icon_owner/owner/member_count/my_role/muted`。`at_me` 由服务端按 `mentions` 与已读游标计算。

**2003 / 2004**：`{"error":0}`。客户端每 10 秒发送一次；服务端 30 秒扫描一次，20 秒无消息判定过期。

**2005 / 2006**：登出后释放租约并删除 `utoken_<uid>`；客户端收到回包后关闭连接。

**2007**：`{"error":0,"uid":1003,"reason":1}`；`reason` 1 顶号、2 登录过期、3 服务端下线。客户端收到后停止自动重连。

## 3.5 好友与资料

| 请求 | 字段 | 响应 |
| --- | --- | --- |
| 3001 | `uid`（纯数字字符串，按 uid 搜；否则按用户名） | 资料 + `relation`：0 非好友、1 好友、2 我已申请、3 对方申请我、4 单向关系（一方已删除）、-1 自己 |
| 3003 | `uid,touid,applyname,bakname,tags[]` | `{error}`；目标在线时服务端推 3005 |
| 3005 | `applyuid,name,nick,desc,icon,sex` | 推送 |
| 3006 | `fromuid,touid,back,tags[]` | 建双向好友 + 单聊会话 + 两条欢迎消息；推 3008 |
| 3009 | `touid` | 只删自己一侧关系，不通知对方 |
| 3011 | `touid,remark,tags[]` | 只影响自己这一侧 |
| 3013 | `touid` | 最新资料 |
| 3015 | `uid,name,nick,desc,sex,icon` | 推给在线好友 |
| 3016 | `nick,sex,desc` | 本人资料更新；操作者取自会话 |

`tags` 是字符串数组，单标签 ≤ 12 字符、最多 10 个。备注 ≤ 64 字节。

## 3.6 单聊与会话

**4001 → 4002**：`{fromuid,touid}` → `{error,uid,thread_id,name,nick,icon,sex}`。会话不存在时创建。

**4003 → 4004**：`{fromuid,thread_id,last_time}`，首次都传 0；服务端每页固定 100 条。响应 `{error,thread_list[],load_more}`，返回的 `thread_id` 与 `time` 可作为下一页游标。

**4005 → 4006**：`{fromuid,thread_id,message_id}`，首次传 0。响应 `{error,thread_id,thread_type,load_more,text_array[]}`，每条：

```json
{"message_id": 12, "thread_id": 1, "fromuid": 1003, "touid": 1004,
 "msgid": "客户端 UUID", "content": "正文", "msg_type": 0,
 "time": 1795148660, "status": 1, "read": true, "recalled": false,
 "mentions": "1002,1003"}
```

`msg_type`：0 文本、1 图片、2 视频、3 文件、4 群系统、7 合并转发。

**4007 → 4008**：`{fromuid,touid,thread_id,thread_type,mentions[],text_array:[{msgid,content,type?}]}`。

- `type` 可省略，只接受 0 与 7；`type=7` 时 `content` 是快照 JSON 且 ≤ 6000 字节。
- 响应 `text_array` 带服务端 `message_id`、`status=1`、`time` 与 `msg_type`。
- 单聊非好友：`error=1014`；群消息无效：`error=1002` 且 `group_invalid=true`。
- 接收方通知 4009 结构相同，`status=3`。

**4010 / 4013**：图片与视频/文件先落库占位。

```json
// 4010
{"fromuid":1003,"touid":1004,"thread_id":1,"thread_type":1,
 "md5":"...","name":"<md5>.jpg","token":"...","unique_id":"...","mentions":[]}
// 4013
{"msg_type":2,"md5":"...","name":"<md5>.mp4","file_name":"原文件名",
 "file_size":123456,"thumb":"<md5>.jpg","content":"...","unique_id":"..."}
```

**4016 → 4017**：`{thread_id,message_id}`；单聊推进游标并推 4018 给对方，群聊只推进自己的游标。

**4019 → 4020**：`{message_id}`，2 分钟内且必须是发送者；回包带 `thread_id/message_id/thread_type`。成功后双方气泡改为"撤回了一条消息"，正文清空。

## 3.7 群聊

**5001 → 5002**：`{fromuid,name,members[]}`；一次最多 50 人，成员必须是好友。响应 `{thread_id,thread_type,group,members[],system_message}`。

**5003 → 5004**：`{thread_id}` → `{group,members[]}`；

```json
"group": {"thread_id":3,"name":"项目群","notice":"","icon":"<md5>.png",
          "icon_owner":1003,"owner":1003,"member_count":3,
          "my_role":2,"muted":false,"my_nick":"","created_at":1795148478}
"members": [{"uid":1001,"name":"西故","nick":"xigu","icon":"","sex":0,
             "role":2,"group_nick":""}]
```

`role`：0 成员、1 管理员、2 群主。

**5005 → 5006**：`{thread_id,action,value}`；`action`：

| action | value | 权限 |
| --- | --- | --- |
| `name` | 1–30 字符 | 管理员及以上 |
| `notice` | ≤ 200 字符 | 管理员及以上 |
| `icon` | `<32 位 md5>.png`，空串表示恢复首字回退 | 管理员及以上 |
| `nickname` | ≤ 16 字符 | 自己 |
| `mute` | `"1"` / `"0"` | 自己 |

**5007 → 5008**：`{thread_id,action,members[],target}`。

| action | 说明 |
| --- | --- |
| `invite` | 只能邀请好友，已在群里的跳过；单群上限 200 |
| `remove` | 群主可移除任何人（自己除外）；管理员只能移除普通成员 |
| `set_admin` / `unset_admin` | 只有群主 |
| `leave` | 群主必须先解散 |
| `dismiss` | 只有群主；软删，成员与消息保留 |

**5009 推送**：`{error,thread_id,thread_type:2,event,...}`；`event` 为 `created / info / members / removed / dismissed / message`。`info` 事件带变化的字段（群名、公告或 `icon + icon_owner`）。

## 3.8 资源通道

独立 TCP，帧头为 **2 字节 ID + 4 字节长度**，包体由"4 字节 JSON 头长度 + JSON + 二进制数据"组成。

| 请求 | 响应 | 用途 |
| --- | --- | --- |
| 6001 | 6002 | 通用文件分片上传 |
| 6003 | 6004 | 查询续传进度 |
| 6005 | 6006 | 分片下载 |
| 6007 | 6008 | 头像上传（最后一片写 `user.icon`） |

**上传请求头**

```json
{"uid":1003,"token":"...","md5":"32 位","name":"<md5>.jpg",
 "seq":1,"trans_size":65536,"total_size":150000,"last":0,
 "message_id":12,"touid":1004,"thread_id":1,"thread_type":1,
 "unique_id":"...","msg_type":1}
```

`message_id / touid / thread_id / thread_type / unique_id / msg_type` 仅聊天资源需要，头像与普通上传省略。

**上传响应**

```json
{"error":0,"md5":"...","name":"...","seq":1,"total_size":150000,
 "trans_size":65536,"last":0,"uid":1003}
```

**同步进度 6003**：`{md5,uid,token}` → `{error,md5,name,seq,trans_size,total_size}`。

**下载请求 6005**：`{uid,token,owner_uid,name,seq}`；响应同为 2+4 帧，JSON 头 `{error,name,seq,is_last,current_size,total_size}` 后跟该片原始字节。

| 约束 | 值 |
| --- | --- |
| 分片大小 | 64 KB |
| 单帧上限 | 128 KB |
| 单文件上限 | 4 GB |
| 续传进度 TTL | 1 小时 |
| 错误码 | 12xx（见 3.11） |

## 3.9 HTTP 接口

| 方法 | 路径 | 请求 | 成功响应 |
| --- | --- | --- | --- |
| GET | `/healthz` | 无 | `{status,redis,mysql,uptime_seconds}` |
| POST | `/get_verifycode` | `{email}` | `{error,email}` |
| POST | `/user_register` | `{email,user,passwd,confirm,verifycode}` | `{error,uid,email,user}` |
| POST | `/reset_pwd` | `{email,user,passwd,verifycode}` | `{error,email,user}`；同时吊销旧令牌 |
| POST | `/user_login` | `{email,passwd}` | `{error,uid,email,token,host,port,res_host,res_port}` |

- `passwd` 是客户端 XOR 变形后的字符串（`123456` → `745230`），服务端直接对它做 Argon2id。
- 邮箱统一转小写后处理；`confirm` 必须与 `passwd` 完全一致。
- 所有接口返回 `Content-Type: application/json`；错误码见 3.11。

## 3.10 gRPC 接口

| 服务 | 方法 | 调用方 → 提供方 |
| --- | --- | --- |
| `VerifyService` | `GetVerifyCode` | Gate → Verify |
| `StatusService` | `GetChatServer` | Gate → Status |
| | `AllocateSession` / `RenewSessions` / `ReleaseSession` / `LogoutSession` | Chat → Status |
| | `LookupRoute` / `BatchLookupRoute` | Chat → Status |
| | `RegisterInstance` / `UnregisterInstance` / `DrainInstance` | Chat → Status |
| `ChatService` | `NotifyTextChatMsg` / `NotifyChatEvent` / `NotifyGroupEvent` | Chat ↔ Chat |
| | `NotifyAddFriend` / `NotifyAuthFriend` / `NotifyProfileChanged` / `BroadcastProfile` | Chat ↔ Chat |
| | `NotifyImageUploaded` | Resource → Chat |
| | `KickSession` | Status → Chat |

proto 文件：`server/proto/message.proto`。服务间 gRPC 全部使用 `InsecureChannelCredentials`，设计前提是只走回环或容器内网。

## 3.11 错误码

| 码 | 名称 | 含义 |
| --- | --- | --- |
| 0 | Success | 成功 |
| 1001 | Error_Json | 请求不是 JSON 对象或字段类型不合法 |
| 1002 | RPCFailed | RPC / 数据库 / 缓存执行失败 |
| 1003 | VerifyExpired | 验证码不存在或已过期 |
| 1004 | VerifyCodeErr | 验证码不匹配 |
| 1005 | UserExist | 用户名或邮箱已注册 |
| 1006 | PasswdErr | 密码错误；注册时表示两次输入不一致 |
| 1007 | EmailNotMatch | 重置密码时用户名与邮箱不匹配 |
| 1008 | PasswdUpFailed | 重置密码写库失败 |
| 1009 | PasswdInvalid | 邮箱与密码不匹配 |
| 1010 | TokenInvalid | 令牌缺失、失效或与 uid 不匹配 |
| 1011 | UidInvalid | uid 不存在或无权操作 |
| 1012 | RecallExpired | 超过可撤回时间 |
| 1013 | RecallDenied | 非发送者或已撤回 |
| 1014 | NotFriend | 双方已不是好友 |
| 1015 | GroupNotFound | 群不存在、已解散或不在群里 |
| 1016 | GroupDenied | 当前角色无权限 |
| 1017 | GroupFull | 群成员已达上限 |
| 1018 | GroupMemberExists | 被邀请人已在群里 |
| 1022 | VerifyTooFrequent | 验证码冷却或小时配额 |
| 1023 | VerifySendFailed | 邮件投递失败 |
| 1101 | ServiceUnavailable | 依赖服务暂不可用 |
| 1102 | SessionFenced | 会话被更高 epoch 顶替 |
| 1103 | SessionExpired | 租约过期 |
| 1104 | InstanceUnknown | 实例未注册或已过期 |
| 1105 | TooManyRequests | 请求过于频繁 |
| 1106 | ResponseTooLarge | 响应超过单帧上限 |
| 1201 | FileNotExists | 传输记录不存在 |
| 1202 | FileSaveRedisFailed | 进度写 Redis 失败 |
| 1203 | FileWritePermissionFailed | 目标文件不可写 |
| 1204 | FileDataInvalid | 分片数据与偏移不一致 |
| 1205 | FileNameInvalid | 文件名非法 |
| 1206 | FileTooLarge | 超过 4 GB |
| 1207 | FileSeqInvalid | 分片序号不连续 |
| 1208 | FileReadPermissionFailed | 源文件不可读 |
| 1209 | FileOffsetInvalid | 偏移越界 |
| 1210 | FileQuotaExceeded | 超出用户配额 |

## 3.12 兼容性

- **只增不改**：新增字段必须可选，客户端缺省按 0/空处理；删除字段前先确认两端版本。
- **消息号不复用**：废弃的号保留空位，避免旧客户端误解新消息。
- **资源协议独立版本**：`server/resources` 与聊天帧解耦，升级资源通道不影响登录与消息。
- 合并转发快照是"展示数据"，不引用原消息 id；原消息被删除或撤回不影响已发出的快照。
