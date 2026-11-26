# ============================================================
#  yunjian_common — 五个服务共用的基础设施（静态库）
#
#  收敛的是各服务原本各自复制一份、且已经发生漂移的模块：
#    ConfigMgr    配置文件读取（cwd/config.ini）
#    IOContextPool asio 事件循环线程池
#    RedisMgr     hiredis 连接池与命令封装
#    Log          spdlog 异步日志（控制台 + 轮转文件）
#    MysqlSession MySQL 会话与语句（Boost.MySQL，经典协议 + 每查询 deadline）
#    MysqlPool    MySQL 连接池与 RAII 借出句柄
#    MysqlGuard   MySQL 健康熔断（探针 + 看门狗）
#    Defer        作用域守卫（头文件）
#    TlsContext   证书加载、到期提示与热加载
#    TlsSocket    客户端连接的传输层：明文或 TLS，上层协议不感知
#    Singleton    进程内单例模板（头文件）
#
#  平台差异只允许出现在本目录：hiredis 在 MSVC 下缺少 timeval 定义，
#  由 RedisMgr.cpp 顶部的 winsock2.h 兼容包含解决，业务代码不再出现平台宏。
# ============================================================

add_library(yunjian_common STATIC
    src/ConfigMgr.cpp
    src/IOContextPool.cpp
    src/Log.cpp
    src/MysqlPool.cpp
    src/MysqlSession.cpp
    src/MysqlGuard.cpp
    src/RedisMgr.cpp
    src/TlsContext.cpp
    src/TlsSocket.cpp
)

# include 目录对使用方可见：各服务仍然写 #include "RedisMgr.h"
target_include_directories(yunjian_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

# OpenSSL 必须是 PUBLIC：TlsSocket.h 里的 ssl::stream 模板会实例化在使用方的编译单元里，
# 头文件路径与 libssl/libcrypto 都要能传到各服务
find_package(OpenSSL REQUIRED)
# Boost.MySQL：经典协议客户端，header-only + boost-charconv
find_package(boost_mysql CONFIG REQUIRED)

target_link_libraries(yunjian_common PUBLIC
    Boost::filesystem
    Boost::headers
    hiredis::hiredis
    spdlog::spdlog
    OpenSSL::SSL
    OpenSSL::Crypto
    Boost::mysql
    Threads::Threads
)
