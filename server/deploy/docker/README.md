# XiguChat 后端 Docker 部署

把「编译环境 + MySQL + Redis」打包成一个镜像。跑镜像的机器不需要装编译器、
不需要访问 GitHub：镜像里的第三方依赖（grpc / boost / hiredis 等）
已经用 vcpkg 预编译好，容器启动后只需从 git 仓库取代码、编译自己的代码。

---

## 1. 准备

- 安装 Docker（服务器装 `docker.io` 即可，无需其他任何东西）
- 本机直接跑源码（不用容器）时，需要改地址或密钥就把模板复制一份再改：

```bash
cp client/config.ini.example client/config.ini
cp server/services/GateServer/config.ini.example server/services/GateServer/config.ini
cp server/services/StatusServer/config.ini.example server/services/StatusServer/config.ini
cp server/services/VerifyServer/config.ini.example server/services/VerifyServer/config.ini
cp server/services/ChatServer/config/ChatServer.ini.example server/services/ChatServer/config/ChatServer.ini
cp server/services/ChatServer/config/ChatServer2.ini.example server/services/ChatServer/config/ChatServer2.ini
```

> 不改也能直接构建运行，CMake 会在缺 `config.ini` 时自动用模板补一份。

---

## 2. 构建镜像

在 `server` 目录下执行（构建上下文就是 `server` 目录）：

```bash
docker build --platform linux/amd64 -f deploy/docker/Dockerfile -t xiguchat-server:amd64 .
```

产物固定是 x86_64 架构，与云服务器一致；在 Apple 芯片的 Mac 上构建时容器以模拟方式
运行，速度比同配置的 Linux 机器慢。首次约 1 小时，时间主要花在 vcpkg 编译第三方依赖，
之后再构建会命中缓存。

网络受限导致容器内下载失败时，可把宿主机代理传进去：

```bash
docker build --platform linux/amd64 --build-arg PROXY=http://host.docker.internal:7890 \
  -f deploy/docker/Dockerfile -t xiguchat-server:amd64 .
```

---

## 3. 配置环境变量

```bash
cd server/deploy/docker
cp .env.example .env
```

编辑 `.env`：

| 变量 | 说明 |
|---|---|
| `PUBLIC_HOST` | **必须填对**。客户端实际用来访问服务器的地址：本机测试用 `127.0.0.1`，公网部署填域名（如 `xiguchat.icu`）。`StatusServer` 会把这个地址原样下发给客户端去连聊天与资源服务 |
| `GATE_PORT` | 网关端口，默认 `8080`（明文 HTTP）；启用 HTTPS 后改成 `8443`，客户端 `config.ini` 的 `port` 也要改成同一个值 |
| `EMAIL_HOST` / `EMAIL_PORT` | SMTP 服务器，默认 `smtp.qq.com` / `465` |
| `EMAIL_USER` / `EMAIL_PASS` | 发信账号与授权码（QQ 邮箱在「设置 → 账号 → POP3/SMTP 服务」生成） |
| `EMAIL_FROM` | 发件人显示地址，通常与 `EMAIL_USER` 相同 |

---

## 4. 启动

```bash
cd server/deploy/docker
docker compose up -d          # 启动
docker compose logs -f        # 看日志，Ctrl+C 退出
docker compose down           # 停止并移除容器（数据卷保留）
```

首次启动容器内依次完成：拉代码 → 初始化 MySQL 并导入 `backup/xigu.sql` → 编译 → 启动六个服务。
看到日志里列出 8080 / 8090 / 8091 / 9090 等端口在监听，即启动成功。

验证服务活着：

```bash
curl http://<PUBLIC_HOST>:8080/healthz
```

### 4.1 启用 HTTPS / TLS

三条客户端通道默认都是明文。把证书放进 `./certs`（与 `docker-compose.yml` 同级），
网关（HTTPS）与聊天、文件通道（TLS）会一起切换，代码不用改：

```bash
mkdir -p certs
# 腾讯云证书下载选 Nginx 格式，解压后改这两个名字
cp xiguchat.icu_bundle.crt certs/fullchain.pem   # 必须是带中间 CA 的整份链
cp xiguchat.icu.key       certs/privkey.pem
chmod 600 certs/privkey.pem

# .env 里改两行
#   PUBLIC_HOST=xiguchat.icu
#   GATE_PORT=8443
docker compose up -d
curl https://<PUBLIC_HOST>:8443/healthz
```

启动日志会打印 `传输加密：HTTPS 网关 …`、`tls cert ready … expires_in_days=89` 以及
`tcp listening on :8090 (tls)` / `:9090 (tls)`。
续期时直接覆盖这两个文件，60 秒内自动换新证书，不用重启。
客户端要同时打开 `[Network] tls=true`，并且 `host` 填域名（证书不认 IP）。
完整步骤（解析、备案、证书申请、常见错误）见 [docs/14-tls-deployment.md](../../docs/14-tls-deployment.md)。

---

## 5. 部署到服务器

服务器不需要代码也不需要编译环境，把镜像搬过去即可：

```bash
# 本地导出
docker save xiguchat-server:amd64 -o xiguchat-server.tar

# 传到服务器
scp xiguchat-server.tar ubuntu@<服务器IP>:/home/ubuntu/

# 服务器上导入并启动
ssh ubuntu@<服务器IP>
cd ~ && docker load -i xiguchat-server.tar
mkdir -p xiguchat && cd xiguchat
# 把 deploy/docker 下的 docker-compose.yml 与填好的 .env 放到这里
docker compose up -d
```

云服务器的安全组 / 防火墙要放行**网关端口（默认 8080，启用 HTTPS 后是 8443）、8090、8091、9090**，否则外面连不上。

---

## 6. 更新代码

代码推到仓库后，在跑容器的机器上执行一条命令即可（拉代码 → 重新编译 → 重启服务）：

```bash
docker exec xiguchat /opt/xiguchat/scripts/update.sh
```

先编译成功才停旧服务，所以编译失败时正在运行的服务不会被打断。
容器重启时是否自动拉最新代码由环境变量 `AUTO_PULL` 控制（默认 `1`）。

---

## 7. 端口

| 端口 | 用途 | 是否对外开放 |
|---|---|---|
| 8080 / 8443 | GateServer 接口（`GATE_PORT`，明文或 HTTPS） | 是 |
| 8090 / 8091 | 两个聊天实例的 TCP 长连接 | 是 |
| 9090 | ResourceServer 头像/文件传输 | 是 |
| 50051 / 50052 / 50055 / 50056 | 服务间 gRPC | 否，仅容器内部 |
| 3306 | MySQL 经典协议（服务与图形客户端都用它） | 否，只绑服务器本机回环 |
| 6380 | Redis | 否，只绑服务器本机回环 |

数据库端口刻意不做公网映射：MySQL / Redis 一旦暴露在公网，会被扫描器持续爆破，
Redis 更是常见的入侵入口（写入 SSH 公钥或定时任务即可拿到 shell），复杂密码也挡不住漏洞。
需要在别的电脑上管理数据库时，用 SSH 隧道，不要开端口：

```bash
ssh -N -L 3306:127.0.0.1:3306 ubuntu@<服务器IP>
```

保持这条命令运行，本机用图形客户端连 `127.0.0.1:3306`（账号 `root`、密码 `123456`）
就等于连到服务器容器里的 MySQL。Navicat / DBeaver 等工具也可在连接设置里直接填
SSH 隧道，效果相同。服务进程连接的也是这条 3306。

---

## 8. 其他环境变量

| 变量 | 默认值 | 说明 |
|---|---|---|
| `XIGU_REPO` | `https://gitee.com/shishangyiwugu/xiguchat.git` | 代码仓库 |
| `XIGU_BRANCH` | `main` | 部署哪个分支（仓库的主分支）；要发布别的分支就改这里 |
| `AUTO_PULL` | `1` | 容器启动时是否拉最新代码 |
| `SKIP_BUILD` | `0` | `1` 表示跳过编译，直接用上次的产物启动 |

---

## 9. 常见问题

- **客户端能登录但连不上聊天服务**：`PUBLIC_HOST` 没填外网能访问的地址。
- **改了端口后客户端连不上**：`GATE_PORT` 与客户端 `config.ini` 的 `port` 必须一致；
  启用 HTTPS 时客户端 `scheme` 还要改成 `https`，`host` 必须填证书上的域名（填 IP 会报证书不匹配）。
- **注册时收不到验证码邮件**：`.env` 里的邮件账号或授权码不对；容器日志里能看到 SMTP 报错。
- **改了配置想生效**：改 `server/deploy/docker/config-templates/` 下的模板后重建镜像。
  直接改容器里 `/workspace/.../config.ini` 没用，每次启动都会被模板覆盖。
- **MySQL 数据在哪**：数据卷 `mysql-data`，删容器不影响数据。
- **MySQL 为什么用官方源装**：项目通过经典协议（3306）访问数据库，X 插件已在
  `mysql.cnf` 里关闭。官方源只为固定小版本行为；换成 Ubuntu 仓库的 `mysql-server`
  同样可行，但需要重新验证初始化流程。
