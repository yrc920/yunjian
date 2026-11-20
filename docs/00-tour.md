# 00 · 项目导览

这份文档不解释所有细节，只回答三个问题：**进程怎么跑起来、一次登录经过哪些代码、出问题先看哪里。**
需要改协议看 `03-protocol.md`，改表看 `04-data-model.md`，部署看 `02-build.md` 与 `14-tls-deployment.md`。

---

## 0.1 五个进程

| 进程 | 默认端口 | 职责 | 入口 |
| --- | --- | --- | --- |
| GateServer | HTTP/HTTPS 8080 | 注册、登录、改密、验证码；签发 token | `services/GateServer/src/GateServer.cpp` |
| VerifyServer | gRPC 50051 | 生成验证码并通过 SMTP 发信 | `services/VerifyServer/src/VerifyServer.cpp` |
| StatusServer | gRPC 50052 | 实例注册、选路、租约/epoch、在线位置 | `services/StatusServer/src/StatusServer.cpp` |
| ChatServer × N | TCP 8090/8091 + gRPC 50055/50056 | 长连接、好友、消息、群聊、跨实例转发 | `services/ChatServer/src/ChatServer.cpp` |
| ResourceServer | TCP 9090 | 头像、图片、视频、文件的分片上传与下载 | `services/ResourceServer/src/ResourceServer.cpp` |

启动顺序：VerifyServer → StatusServer → ChatServer（各实例）→ ResourceServer → GateServer。
每个服务按**当前工作目录**读 `config.ini`，必须进入自己的产物目录启动。

---

## 0.2 一次登录走哪些代码

1. **HTTP 取票**：客户端 `AuthService::login` → `POST /user_login`。
   GateServer `LogicSystem` 解析并归一化邮箱 → `PasswordPool` 线程池里
   `MysqlDao::CheckPwd`（Argon2 校验）→ `StatusGrpcClient::GetChatServer`。
2. **选路与 token**：StatusServer `InstanceRegistry::Pick` 按 `sessions/capacity` 选实例，
   `IssueToken` 写 `utoken_<uid>`（7 天）。
3. **建立长连接**：客户端拿到 `host/port/token` → `TcpMgr::ConnectToServer` →
   发 2001 `{uid, token}`。
4. **ChatServer 认证**：`LogicSystem::LoginHandler` 读 `utoken_` 快速校验 →
   `UserCache::Get` 取资料 → 拉好友/申请/会话首页 → `AllocateSession` 申请租约拿 epoch →
   `SetEpoch + SetUserId + UserMgr::SetUserSession` → 回 2002。
5. **登录后**：客户端 `SessionService::onChatLoggedIn` 写 `UserMgr`、打开本地 SQLite、
   按 `sync_cursor` 拉增量消息；`ChatService` 负责后续发送与资源上传。

关键边界：**租约申请在资料加载之后**，新设备登录失败不会把旧设备踢掉；
旧连接由 StatusServer 异步 `KickSession`，即使通知丢失也会在下一次续租收到 Fenced。

---

## 0.3 三条客户端通道

| 通道 | 帧格式 | 代码 |
| --- | --- | --- |
| HTTP | 标准 HTTP/JSON | `client/network/httpmgr.cpp`、`GateServer` |
| Chat TCP | 2 字节 ID + 2 字节长度 + JSON | `client/network/tcpmgr.cpp`、`ChatServer` |
| Resource TCP | 2 字节 ID + 4 字节长度 +（4 字节头长 + JSON + 分片） | `client/network/filetcpmgr.cpp`、`ResourceServer` |

三条通道都支持 TLS；开关在 `config.ini`，客户端由 `scheme` 与 `[Network] tls` 决定。

---

## 0.4 日志

- 目录：各服务产物目录下的 `logs/<服务名>.log`，按大小轮转，控制台同时输出。
- 级别：`config.ini` 的 `[Log] Level`，排障时临时改 `debug`。
- 常用关键字：
  - `chat login uid=... epoch=... inst=...`：登录成功与落点。
  - `[login] ... superseded`：顶号。
  - `[lease] fenced`：续租发现被更高 epoch 顶替。
  - `[route] batch lookup failed`：StatusServer 批量位置查询失败。
  - `[delivery] queue full`：跨实例投递积压，实时性下降但消息已落库。
  - `[archive] moved ...`：归档批次。
  - `[resource] quota exceeded` / `download denied`：配额与鉴权。

---

## 0.5 按症状排查

| 症状 | 先看 | 常见原因 |
| --- | --- | --- |
| 登录返回 1002 | GateServer 日志、`/healthz` | MySQL/Redis 不可用，或 StatusServer 未启动 |
| 登录返回 1104 | StatusServer 日志 | ChatServer 未注册、注册过期或被 drain |
| 登录返回 1105 | GateServer/StatusServer 日志 | 登录失败次数或分配频率超限 |
| 连上 ChatServer 立即断开 | ChatServer 日志 | 帧头 ID/长度非法、token 不匹配、心跳超时 |
| 消息发送成功但对方收不到 | 发送方 `[delivery]`、接收方在线位置 | 对方离线、路由缓存过期、跨实例 RPC 失败；消息仍在库里 |
| 图片一直转圈 | ResourceServer 日志、`upload_state` | 上传未完成、配额超限、完成通知失败（兜底应回写 `upload_state=1`） |
| 群聊消息 1015 | ChatServer 日志 | 不在群里、群已解散、`thread_id` 与成员表不匹配 |
| 客户端反复重连 | 2007 的 reason | 被顶号（reason=1）不应重连；网络断开才退避重连 |

---

## 0.6 本地最小启动

```bash
# 依赖
docker start mysql redis

# 洗库
docker exec -i mysql mysql -uroot -p123456 -e \
  "DROP DATABASE IF EXISTS xigu; CREATE DATABASE xigu CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;"
docker exec -i mysql mysql -uroot -p123456 xigu < backup/xigu.sql

# 后端
cd server && export VCPKG_ROOT=~/vcpkg
cmake --preset macos-clang && cmake --build --preset macos-clang
cd build/macos-clang/bin
(cd VerifyServer && ./VerifyServer) &
(cd StatusServer && ./StatusServer) &
(cd ChatServer && ./ChatServer) &
(cd ChatServer2 && ./ChatServer) &
(cd ResourceServer && ./ResourceServer) &
(cd GateServer && ./GateServer) &

# 测试
cd server && ctest --preset macos-clang --output-on-failure
```

客户端：`cd client && cmake --preset macos-clang && cmake --build --preset macos-clang`。
