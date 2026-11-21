# 15 · 排障手册

按症状查。每条给出**先看哪里**与**常见原因**，命令默认在服务器或本机仓库根目录执行。

## 15.1 先做三件事

```bash
# 1. 服务是否健康
curl -s http://127.0.0.1:8080/healthz

# 2. 端口是否在听
ss -lntp | grep -E '8080|8090|8091|9090|50051|50052|50055|50056'

# 3. 最近日志
tail -n 50 server/build/macos-clang/bin/ChatServer/logs/ChatServer.log
```

`/healthz` 返回 `{"status":"ok","redis":true,"mysql":true}` 说明网关与两个依赖正常；`degraded` 时先修依赖。

## 15.2 登录

| 现象 | 先看 | 常见原因 |
| --- | --- | --- |
| 连接被拒绝 | 网关是否监听、地址端口 | 服务没起；`PUBLIC_HOST`/端口填错；防火墙 |
| `1009` 密码错误 | 账号是否来自当前库 | 导入的是 `xigu_clean.sql`；密码不是 `123456`；客户端 XOR 未生效 |
| `1010` 令牌失效 | 是否刚登出/改密/被顶号 | 自动登录令牌过期；重置密码后旧令牌被吊销 |
| `1105` 登录过于频繁 | StatusServer 日志 | 10 秒内同一 uid 超过 5 次；客户端重连风暴 |
| `1022` 验证码频繁 | Redis `code_cd_*` | 60 秒冷却或小时配额 |
| `1023` 邮件失败 | VerifyServer 日志 | SMTP 授权码错误；465 被防火墙拦；CA 配置 |
| `1005` 已注册 | `user` 表 | 用户名或邮箱重复 |
| `1101` 服务不可用 | StatusServer / Redis | 没有可用实例；Redis 不可用 |

```bash
# 看令牌与租约
docker exec redis redis-cli -a 123456 --no-auth-warning ttl utoken_1003
docker exec redis redis-cli -a 123456 --no-auth-warning hgetall sess_1003
docker exec redis redis-cli -a 123456 --no-auth-warning smembers instances
```

## 15.3 登录成功但连不上聊天

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 一直"正在连接" | 8090/8091 未放行 | 安全组放行；`ss` 确认监听 |
| `1101` / `1104` | 实例未注册或已过期 | 看 ChatServer 启动日志的 `register ok`；检查白名单 |
| TLS 握手失败 | 客户端 `tls` 与服务端不一致 | 两边同时开或同时关 |
| 证书不匹配 | 客户端用 IP 连接 | `host` 改成证书域名 |
| 立即收到 2007 | 账号在别处登录 | 单端登录，属于预期 |

```bash
# 实例注册
docker exec redis redis-cli -a 123456 --no-auth-warning keys 'inst_*'
docker exec redis redis-cli -a 123456 --no-auth-warning hgetall inst_chatserver1
```

## 15.4 消息

| 错误 / 现象 | 含义 | 处理 |
| --- | --- | --- |
| `1014` | 双方已不是好友 | 重新加好友；单向删除后必须双方恢复 |
| `1002 + group_invalid` | 不在群或群已解散 | 刷新群列表；确认 `conversation.state=0` |
| `1105` | 消息发送过快 | 降低频率；检查客户端是否循环重发 |
| 消息发出但对方收不到 | 对方离线 / 路由失效 | 消息已落库，对方登录可见；看 `SessionRouter` 日志 |
| 重启后消息顺序乱 | 客户端按 `time` 排序 | 以 `message_id` 为准检查 `created_at` |

```sql
-- 最近消息
SELECT message_id, thread_id, sender_id, msg_type, state,
       UNIX_TIMESTAMP(created_at) FROM chat_message
ORDER BY message_id DESC LIMIT 20;
```

## 15.5 群聊

| 错误 | 含义 |
| --- | --- |
| `1015` | 群不存在、已解散或不在群里 |
| `1016` | 当前角色无权限（改群名、踢人、设管理员等） |
| `1017` | 成员达到 200 上限 |
| `1018` | 被邀请人已在群里 |

排查顺序：`conversation.state` → `conversation_member.role` → 操作类型是否匹配权限矩阵。

## 15.6 资源

| 现象 | 检查 |
| --- | --- |
| 上传 `1205` | 资源名不是 32 位 MD5，或文件名含路径分隔符 |
| 上传 `1207` | 分片序号不连续；客户端并发窗口过大 |
| 上传 `1210` | 超出 `QuotaMB`；清理用户目录或调大配额 |
| 上传中断无法续传 | `file_upload_<md5>` 是否还在 1 小时 TTL 内 |
| 下载 `1201` | 文件未落盘；请求者与归属人不共处会话；已被清理 |
| 头像不更新 | `user.icon` 是否写入；3015 是否广播；客户端缓存是否命中旧文件 |
| 磁盘持续增长 | 检查 `.part` 僵尸文件与历史资源；调整 `StaleUploadHours` |

```bash
# 存储占用
du -sh server/build/macos-clang/bin/ResourceServer/storage/*
# 进度键
docker exec redis redis-cli -a 123456 --no-auth-warning keys 'file_upload_*'
```

## 15.7 TLS

| 现象 | 原因 |
| --- | --- |
| 启动即退出，日志 `SslCert and SslKey must be set together` | 只配了一个文件 |
| 浏览器/客户端证书错误 | 证书链不完整；域名与证书 SAN 不一致 |
| `stream_truncated` / 连接被重置 | 用明文访问了 TLS 端口，或反之 |
| 续期后仍旧证书 | 文件未覆盖到挂载目录；检查 mtime 与 `SslReloadSeconds` |

```bash
openssl s_client -connect <域名>:8443 -servername <域名> </dev/null 2>/dev/null \
  | openssl x509 -noout -subject -dates
```

## 15.8 Docker

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| 容器反复重启 | 首次编译失败 | `docker compose logs` 看 CMake/vcpkg |
| 改了配置不生效 | 模板在镜像里 | 重建镜像，或直接改容器内 `config.ini` 仅用于临时验证 |
| 代码不是最新 | `AUTO_PULL=0` 或分支不对 | `docker exec xiguchat /opt/xiguchat/scripts/update.sh` |
| 洗库后数据还在 | 没删卷 | `docker compose down -v` 再 `up -d` |
| 登录成功但客户端连不上 | `PUBLIC_HOST` 是容器地址 | 改成公网域名/IP |

## 15.9 客户端

| 现象 | 处理 |
| --- | --- |
| 启动即退出 | 看 `logs/client.log` 最后 20 行；确认 `config.ini` 与程序同目录 |
| 界面空白 | QML 加载失败；Debug 构建看控制台 `qml` 报错 |
| 一直"恢复上次登录" | 令牌失效但网络异常；删除偏好设置里的 `AutoLogin` 或重新登录 |
| 收不到系统通知 | 系统通知权限；macOS 首次需要授权 |
| 本地搜索无结果 | 本地缓存被关闭或未同步；检查 `[Storage] localCache` |

日志位置：

| 平台 | 路径 |
| --- | --- |
| macOS | `~/Library/Application Support/XiguChat/xiguchat/logs/client.log` |
| Windows | `%APPDATA%\XiguChat\xiguchat\logs\client.log` |

## 15.10 日志级别

临时打开调试：

```ini
[Log]
Level = debug
```

- 服务端改各自 `config.ini`，重启生效；
- 客户端改 `client/config.ini`，重启生效；
- 复现后改回 `info`，避免日志快速增长。

## 15.11 快速诊断脚本

```bash
#!/usr/bin/env bash
set -u
echo "== health =="; curl -s http://127.0.0.1:8080/healthz; echo
echo "== ports =="; ss -lntp 2>/dev/null | grep -E '8080|8090|8091|9090' || true
echo "== instances =="; docker exec redis redis-cli -a 123456 --no-auth-warning smembers instances
echo "== chat log =="; tail -n 20 server/build/macos-clang/bin/ChatServer/logs/ChatServer.log
```
