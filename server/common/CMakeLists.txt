# ============================================================
#  xiguchat_common — 五个服务共用的基础设施（静态库）
#
#  收敛的是各服务原本各自复制一份、且已经发生漂移的模块：
#    ConfigMgr    配置文件读取（cwd/config.ini）
#    IOContextPool asio 事件循环线程池
#    RedisMgr     hiredis 连接池与命令封装
#    Singleton    进程内单例模板（头文件）
#
#  平台差异只允许出现在本目录：hiredis 在 MSVC 下缺少 timeval 定义，
#  由 RedisMgr.cpp 顶部的 winsock2.h 兼容包含解决，业务代码不再出现平台宏。
# ============================================================

add_library(xiguchat_common STATIC
    src/ConfigMgr.cpp
    src/IOContextPool.cpp
    src/RedisMgr.cpp
)

# include 目录对使用方可见：各服务仍然写 #include "RedisMgr.h"
target_include_directories(xiguchat_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(xiguchat_common PUBLIC
    Boost::filesystem
    Boost::headers
    hiredis::hiredis
    Threads::Threads
)
