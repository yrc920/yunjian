# 02 · 构建与运行

## 2.1 两个独立工程

| 工程 | 目录 | 标准 | 依赖来源 |
| --- | --- | --- | --- |
| 客户端 | `client/` | C++17 | 本机 Qt 6.5（`QT_ROOT` 或 Qt Creator Kit） |
| 服务端 | `server/` | C++20 | vcpkg manifest（`server/vcpkg.json`，baseline 锁定版本） |

两端都用 CMakePresets，命令行、Qt Creator、Visual Studio 共用一份预设。**服务端要求三平台可构建；客户端只支持 Windows / macOS**，Linux 客户端预设保留但不属于发布范围。

## 2.2 前置

| 工具 | 要求 | 用途 |
| --- | --- | --- |
| CMake | ≥ 3.21（后端预设要求 ≥ 3.27） | 预设与运行时 DLL 收集 |
| Ninja | 任意近期版本 | macOS / Linux 生成器 |
| Visual Studio | 2026 工具链 | Windows 后端 |
| vcpkg | 与 `builtin-baseline` 兼容 | 后端依赖 |
| Qt | 6.5.x：Core / Gui / Widgets / Network / Quick / QuickControls2 / QuickDialogs2 | 客户端 |
| MinGW | Qt 6.5 自带 `mingw1120_64` | Windows 客户端 |

macOS / Linux 上 `libsodium` 端口走 autotools，需预装：

```bash
# macOS
brew install autoconf autoconf-archive automake libtool
# Debian / Ubuntu
sudo apt install autoconf autoconf-archive automake libtool
```

## 2.3 后端构建

```bash
cd server
export VCPKG_ROOT=/path/to/vcpkg      # Windows: set VCPKG_ROOT=D:\vcpkg
cmake --preset macos-clang            # 或 windows-msvc / linux-gcc
cmake --build --preset macos-clang
```

预设：

| 预设 | 平台 | 说明 |
| --- | --- | --- |
| `windows-msvc`（构建用 `windows-msvc-debug` / `windows-msvc-release`） | Windows | 多配置生成器，产物在 `bin/<服务>/<Config>/` |
| `macos-clang` / `macos-clang-release` | macOS | Ninja 单配置 |
| `linux-gcc` / `linux-gcc-release` | Linux | Ninja 单配置 |
| `linux-docker` | Linux 容器 | 依赖预装到 `/opt/xiguchat/vcpkg_installed`，带 ccache |

如果依赖已经装在固定目录，可复用而不触发重新安装：

```bash
cmake --preset macos-clang \
  -DVCPKG_MANIFEST_INSTALL=OFF \
  -DVCPKG_INSTALLED_DIR=/path/to/vcpkg_installed
```

### proto 代码生成

`server/proto/message.proto` 是全仓唯一来源，构建期生成 C++ 代码并打包为静态库 `xiguchat_proto`。改协议字段只需改这一份，然后重新构建；生成物不进入仓库。

## 2.4 ChatServer 多实例

`services/ChatServer/config/` 下每份 `*.ini` 对应一个实例：

| 配置 | 实例名 | 客户端端口 | gRPC 端口 |
| --- | --- | --- | --- |
| `ChatServer.ini` | `chatserver1` | 8090 | 50055 |
| `ChatServer2.ini` | `chatserver2` | 8091 | 50056 |

构建时按文件名排序取第一份作为主实例，其余实例目录整目录复制主实例产物，再覆盖自己的 `config.ini`：

| 产物目录 | 使用的配置 |
| --- | --- |
| `build/<preset>/bin/ChatServer/` | `config/ChatServer.ini` |
| `build/<preset>/bin/ChatServer2/` | `config/ChatServer2.ini` |

两个目录各自包含可执行文件与 `config.ini`，启动时进入哪个目录就是哪个实例。

新增实例：

1. 复制一份 `ChatServer2.ini.example`，改 `Name / Port / RPCPort / PeerServer`；
2. 在 StatusServer 的 `[chatservers] Name` 白名单里加上新名字；
3. 在各实例的 `[PeerServer] Servers` 与 `[<对端名>]` 段补上地址；
4. 重新配置构建（`CONFIGURE_DEPENDS` 会自动发现新文件）。

## 2.5 客户端构建

```bash
cd client
export QT_ROOT=~/Qt/6.5.3/macos     # Windows 另设 QT_MINGW_ROOT
cmake --preset macos-clang           # Windows: windows-mingw
cmake --build --preset macos-clang
```

Qt Creator 用户直接打开 `client/CMakeLists.txt`，选择 Qt 6.5 Kit 即可。

客户端 UI 是纯 QML / Qt Quick；`QApplication` 与 `QSystemTrayIcon` 只提供托盘与通知能力，不创建 QWidget 页面。macOS 的标题栏外观由 `bridge/windowchrome_mac.mm`（Objective-C++）处理，Windows 使用空实现。

## 2.6 产物布局

| 平台 | 后端 | 客户端 |
| --- | --- | --- |
| macOS | `server/build/<preset>/bin/<服务或实例>/` | `client/build/<preset>/bin/xiguchat.app` |
| Linux | `server/build/<preset>/bin/<服务或实例>/` | 不发布 |
| Windows | `server/build/<preset>/bin/<服务或实例>/<Config>/` | `client/build/<preset>/bin/xiguchat.exe` |

每个服务目录都包含可执行文件与 `config.ini`。服务按**当前工作目录**读取配置，因此必须进入自身目录启动。

## 2.7 测试

```bash
cd server
cmake --preset macos-clang -DBUILD_TESTING=ON
cmake --build --preset macos-clang
ctest --preset macos-clang
```

- `-L unit` 只跑单元测试，秒级，不依赖服务；
- 集成测试需要本地栈（Gate + Chat + Resource + MySQL + Redis），端口不通时记 `Skipped`；
- 部署构建加 `-DBUILD_TESTING=OFF`。

详见 [14 测试](14-testing.md)。

## 2.8 打包

### 客户端

| 平台 | 命令 |
| --- | --- |
| Windows | `windeployqt --qmldir client/qml bin/xiguchat.exe`，随后整目录压缩 |
| macOS | `macdeployqt bin/xiguchat.app -qmldir=client/qml -dmg`（可选签名/公证） |

Qt 6.5 自带 QtQuick Controls 与虚拟键盘之外的最小依赖；`--qmldir` 必须指向 `client/qml`，否则运行时找不到自定义模块。

### 服务端

推荐直接使用 Docker 镜像，不单独分发二进制：

```bash
cd server
docker build --platform linux/amd64 -f deploy/docker/Dockerfile -t xiguchat-server:amd64 .
```

镜像内已包含编译工具链、MySQL 8、Redis 与部署脚本；业务代码在容器启动时从仓库拉取并编译。详见 [12 部署与运维](12-deployment.md)。

## 2.9 平台差异

| 维度 | Windows | macOS | Linux |
| --- | --- | --- | --- |
| 生成器 | Visual Studio（多配置） | Ninja | Ninja |
| 后端 triplet | `x64-windows`（动态） | `arm64-osx`（静态为主） | `x64-linux`（静态为主） |
| 运行时库 | 依赖 DLL 拷到可执行文件目录 | rpath / 系统路径 | rpath / 系统路径 |
| 客户端编译器 | MinGW 11.2 | AppleClang | 不发布 |
| 客户端产物 | `xiguchat.exe` | `xiguchat.app` | — |
| CA 证书 | 无系统 PEM，依赖 OpenSSL 默认目录 | `/etc/ssl/cert.pem` | `/etc/ssl/certs/ca-certificates.crt` |

## 2.10 常见问题

| 现象 | 处理 |
| --- | --- |
| 找不到 vcpkg | 设置 `VCPKG_ROOT` 后重开终端或 IDE |
| 找不到 Qt 6.5 | 设置 `QT_ROOT`，或在 Qt Creator 里选 Kit |
| MinGW 报错 | `QT_MINGW_ROOT` 必须是 Qt 6.5 自带的 `mingw1120_64` |
| 新增 ini 没生成目录 | `CONFIGURE_DEPENDS` 偶尔不触发，手动重跑 `cmake --preset ...` |
| 启动即报配置缺失 | 确认在服务自己的产物目录内启动，且 `config.ini` 存在 |
| 链接报 `boost_charconv` 未定义 | 确认 vcpkg 已安装 `boost-mysql` 及其依赖 |
