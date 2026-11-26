# Docker 部署入口

这个目录提供 YunJian 后端的单容器部署：镜像内包含编译工具链、MySQL 8、Redis 与全部服务，服务器只需要 Docker。完整的部署、TLS、洗库与升级说明在 [docs/12-deployment.md](../../../docs/12-deployment.md)。

## 目录内容

| 文件 | 作用 |
| --- | --- |
| `Dockerfile` | 构建镜像：工具链、vcpkg 依赖、MySQL、Redis、脚本与配置模板 |
| `docker-compose.yml` | 单容器编排、端口映射、数据卷与证书挂载 |
| `.env.example` | 环境变量模板 |
| `config-templates/` | 六个进程（五个服务）的 `config.ini` 模板，启动时按环境变量渲染 |
| `scripts/` | 取代码、初始化数据库、编译、启动、更新 |
| `mysql.cnf` / `triplets/` | MySQL 经典协议配置与 vcpkg overlay triplet |

## 快速开始

```bash
# 1. 构建镜像（上下文是 server/）
cd server
docker build --platform linux/amd64 -f deploy/docker/Dockerfile -t yunjian-server:amd64 .

# 2. 配置
cd deploy/docker
cp .env.example .env
# 至少把 PUBLIC_HOST 改成客户端能访问的地址

# 3. 启动
docker compose up -d
docker compose logs -f
```

首次启动依次完成：启动 Redis → 拉取代码 → 初始化 MySQL 并导入 `backup/yunjian.sql` → 编译 → 渲染配置 → 启动六个服务进程。

健康检查：

```bash
curl http://<PUBLIC_HOST>:8080/healthz
```

## 数据与洗库

| 卷 | 内容 |
| --- | --- |
| `mysql-data` | MySQL 数据目录 |
| `workspace` | 拉取的代码与构建产物 |
| `resource-data` | 头像与聊天资源 |

按全新环境重来：

```bash
docker compose down -v
docker compose up -d
```

只洗数据库、保留资源文件：

```bash
docker exec -i yunjian bash -c "
  mysql -uroot -p123456 -e 'DROP DATABASE yunjian;
    CREATE DATABASE yunjian DEFAULT CHARACTER SET utf8mb4;' &&
  mysql -uroot -p123456 yunjian < /workspace/backup/yunjian.sql"
docker compose restart
```

`backup/yunjian.sql` 含演示数据；`backup/yunjian_clean.sql` 是干净库。两份结构一致。

## TLS

把证书放到本目录的 `certs/`：

| 文件 | 内容 |
| --- | --- |
| `certs/fullchain.pem` | 叶子证书 + 中间 CA 的完整链 |
| `certs/privkey.pem` | 无口令私钥 |

`.env` 设置 `PUBLIC_HOST=域名`、`GATE_PORT=8443`，重启容器即启用 HTTPS 与两条 TCP 通道的 TLS。证书续期覆盖文件后默认 60 秒热加载。

## 更新

```bash
docker exec yunjian /opt/yunjian/scripts/update.sh
```

脚本会拉取代码、编译、停旧服务、启动新服务；编译失败时旧服务不受影响。**修改了 `config-templates/` 或 `scripts/` 时必须重建镜像**，只更新代码不会更新镜像内的模板。

## 端口

| 端口 | 用途 | 公网 |
| --- | --- | --- |
| 8080 / 8443 | GateServer | 是 |
| 8090 / 8091 | ChatServer | 是 |
| 9090 | ResourceServer | 是 |
| 50051–50056 | 服务间 gRPC | 否 |
| 3306 / 6380 | MySQL / Redis，只绑回环 | 否 |

更详细的域名、备案、证书、备份与排障见 [docs/12-deployment.md](../../../docs/12-deployment.md)。
