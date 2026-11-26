# 05 · 配置参考

## 5.1 读取规则

- 每个服务从**当前工作目录**读取 `config.ini`，所以必须进入自身产物目录启动。
- 仓库只提交 `*.ini.example`；首次构建会复制一份 `config.ini`，运行前按环境修改。
- 配置项缺省时使用代码内的安全默认值；**证书两项只填一项会直接启动失败**，不会静默退回明文。
- 日志段 `[Log]` 在所有服务中语义一致。

## 5.2 GateServer

路径：`bin/GateServer/config.ini`

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `GateServer` | `Port` | 8080 | HTTP/HTTPS 监听端口 |
| | `SslCert` / `SslKey` | 空 | 都填才启用 HTTPS；`fullchain` + 无口令私钥 |
| | `SslReloadSeconds` | 60 | 证书文件变化检查间隔 |
| `Password` | `Threads` | 2 | Argon2id 并发数，每个约 64 MiB |
| | `QueueLimit` | 128 | 排队上限，满后回 `1101` |
| `VerifyServer` | `Host` / `Port` | 127.0.0.1:50051 | 验证码服务地址 |
| `StatusServer` | `Host` / `Port` | 127.0.0.1:50052 | 状态服务地址 |
| `Mysql` | `Host` / `Port` | 127.0.0.1:3306 | 经典协议 |
| | `QueryTimeoutMs` | 5000 | 单条查询 deadline |
| | `User` / `Passwd` / `Schema` | root / 123456 / yunjian | 数据库账号 |
| `Redis` | `Host` / `Port` / `Passwd` | 127.0.0.1:6380 / 123456 | 缓存 |
| `Log` | `Level` | info | trace/debug/info/warn/error/critical/off |
| | `Dir` | logs | 相对当前工作目录 |
| | `FileMaxMB` / `FileCount` | 32 / 5 | 轮转大小与份数 |
| | `Console` | 1 | 是否同时输出控制台 |

## 5.3 StatusServer

路径：`bin/StatusServer/config.ini`

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `StatusServer` | `Host` / `Port` | 127.0.0.1:50052 | gRPC 监听 |
| `ResourceServer` | `Host` / `Port` | 127.0.0.1:9090 | **下发给客户端的资源地址**，必须是客户端可达地址 |
| `Redis` | 同 GateServer | | 租约、令牌、实例表 |
| `chatservers` | `Name` | chatserver1,chatserver2 | 允许注册的实例名，逗号分隔；留空表示接受任意实例，仅用于调试 |
| `Log` | 同上 | | |

> 生产环境把 `[ResourceServer] Host` 改成公网域名或 IP；`127.0.0.1` 只适用于客户端与服务同机。

## 5.4 VerifyServer

路径：`bin/VerifyServer/config.ini`

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `VerifyServer` | `Host` / `Port` | 0.0.0.0:50051 | gRPC 监听 |
| `Redis` | 同 GateServer | | 验证码写入 |
| `Email` | `Host` / `Port` | smtp.qq.com:465 | SMTP over SSL |
| | `User` / `Pass` | 空 | 发信账号与授权码 |
| | `From` | 空 | 发件人显示地址 |
| | `CA` | 空 | 留空自动探测系统 CA；私有 CA 才填绝对路径 |
| `Log` | 同上 | | |

验证码有效期固定 600 秒，长度 4 位，由代码常量控制，不通过配置修改。

## 5.5 ChatServer

路径：每个实例一个文件，例如 `services/ChatServer/config/ChatServer.ini`（构建后变成 `bin/ChatServer/config.ini`）。

| 段 | 键 | 说明 |
| --- | --- | --- |
| `GateServer` | `Port` | 网关端口，仅用于日志/兼容，不影响长连接 |
| `VerifyServer` | `Host` / `Port` | 同上 |
| `StatusServer` | `Host` / `Port` | 租约与选路 |
| `SelfServer` | `Name` | 实例名，必须与 StatusServer 白名单一致 |
| | `Host` | 监听地址，必须 `0.0.0.0` |
| | `AdvertiseHost` | 下发给客户端的地址 |
| | `AdvertiseRpcHost` | 实例间 RPC 地址，容器内用回环 |
| | `Capacity` | 期望承载会话数，参与选路 |
| | `Port` / `RPCPort` | 客户端 TCP 与实例间 gRPC |
| | `SslCert` / `SslKey` / `SslReloadSeconds` | 与网关相同规则 |
| `Mysql` / `Redis` | 同上 | |
| `PeerServer` | `Servers` | 对端实例名，逗号分隔 |
| `<对端名>` | `Name` / `Host` / `Port` | 对端 RPC 地址 |
| `Archive` | `Enabled` | 1 开启归档，0 关闭 |
| | `RetentionDays` | 主表保留天数，默认 365 |
| | `IntervalHours` | 执行间隔，默认 24 |
| | `BatchSize` | 每批条数，默认 2000 |
| | `FirstDelaySeconds` | 启动后首次延迟，默认 300 |
| `Log` | 同上 | |

新增实例的完整步骤见 [02 构建与运行](02-build.md#24-chatserver-多实例)。

## 5.6 ResourceServer

路径：`bin/ResourceServer/config.ini`

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `SelfServer` | `Port` | 9090 | 客户端资源通道端口 |
| | `SslCert` / `SslKey` / `SslReloadSeconds` | 空 / 60 | TLS 规则同上 |
| `Storage` | `Path` | storage | 资源根目录；头像/文件存 `<Path>/<uid>/<资源名>` |
| | `QuotaMB` | 1024 | 每用户上限，0 表示不限；上传首片校验 |
| | `StaleUploadHours` | 24 | `.part` 僵尸文件保留时长 |
| | `SweepMinutes` | 60 | 清理线程巡检间隔 |
| `Mysql` / `Redis` | 同上 | | |
| `StatusServer` | `Host` / `Port` | 127.0.0.1:50052 | 查询接收方在线位置 |
| `ChatServers` | `Servers` | chatserver1,chatserver2 | 实例名列表 |
| `<实例名>` | `Host` / `Port` | 127.0.0.1:5005x | 各实例 gRPC 地址 |
| `Log` | 同上 | | |

## 5.7 客户端

路径：可执行文件同目录 `config.ini`（Windows 为 `bin/`，macOS 为 `.app/Contents/MacOS/`）。

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `GateServer` | `scheme` | http | `http` / `https` |
| | `host` / `port` | 127.0.0.1 / 8080 | `port` 留空表示 80/443 |
| `Network` | `tls` | false | 聊天与资源通道是否 TLS，必须与服务端证书开关一致 |
| `Log` | `level` | 模板 info；配置缺省时 Debug=debug / Release=info | 客户端日志级别 |
| | `console` | 模板 false；配置缺省时 Debug=true / Release=false | 是否输出控制台 |
| | `file` | true | 是否写 `client.log` |
| | `rules` | 空 | 透传 `QT_LOGGING_RULES`，例如 `xc.net.debug=false` |
| `Storage` | `localCache` | true | false 表示不落盘，适合共享电脑 |

> 开启 TLS 后 `host` 必须是证书上的域名，填 IP 会因证书不匹配被拒。

## 5.8 Docker 环境变量

`server/deploy/docker/.env` 控制容器行为：

| 变量 | 默认 | 作用 |
| --- | --- | --- |
| `PUBLIC_HOST` | 127.0.0.1 | 客户端访问地址，写入聊天与资源的下发地址 |
| `GATE_PORT` | 8080 | 网关端口；HTTPS 时改为 8443 |
| `EMAIL_HOST` / `EMAIL_PORT` | smtp.qq.com / 465 | SMTP |
| `EMAIL_USER` / `EMAIL_PASS` | 空 | 授权码 |
| `EMAIL_FROM` | `EMAIL_USER` | 发件人 |
| `YUNJIAN_REPO` / `YUNJIAN_BRANCH` | Gitee 仓库 / main | 容器启动时拉取的代码 |
| `AUTO_PULL` | 1 | 启动时是否拉最新代码 |
| `SKIP_BUILD` | 0 | 1 表示跳过编译，直接使用上次产物 |

## 5.9 常见变更

| 目标 | 操作 |
| --- | --- |
| 启用 HTTPS / TLS | 证书放入 `certs/fullchain.pem` 与 `certs/privkey.pem`；`GATE_PORT=8443`；客户端 `scheme=https`、`tls=true`、`host` 用域名 |
| 更换网关端口 | 改 GateServer `Port` 与客户端 `port`；Docker 同步改 `GATE_PORT` |
| 新增聊天实例 | 新建 `ChatServer3.ini`，登记 StatusServer `[chatservers]`，补各实例 `[PeerServer]` |
| 调整用户配额 | 改 ResourceServer `[Storage] QuotaMB`，重启资源服务 |
| 关闭本地缓存 | 客户端 `[Storage] localCache=false` |
| 邮件发不出去 | 检查 `EMAIL_PASS` 是否为授权码；`CA` 留空；看 VerifyServer 日志的 SMTP 错误 |
| 临时排障 | 各服务 `[Log] Level=debug`，复现后改回 `info` |
