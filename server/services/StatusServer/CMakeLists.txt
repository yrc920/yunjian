# ============================================================
#  StatusServer — 用户在线状态与路由聚合（gRPC）
#  登录时签发 token 写入 Redis，并按各聊天实例的在线连接数挑选目标实例
#
#  依赖: yunjian_proto（共享 proto 库）+ gRPC/protobuf + Boost
#        + hiredis + jsoncpp（MySQL 走 yunjian_common 的 Boost.MySQL）
#  产物: 单配置为 bin/StatusServer/，多配置为 bin/StatusServer/<配置名>/
#        macOS/Linux 预设使用单配置 Ninja，Windows 预设使用多配置 Visual Studio
# ============================================================

# 可执行文件名（同时用于输出目录与部署）
set(SERVER_NAME StatusServer)

# ---------- 源文件清单 ----------
# StatusServer 是会话生命周期的唯一权威：
#   SessionRegistry  租约与围栏令牌（Redis 原子脚本）
#   InstanceRegistry 实例注册、心跳、负载与下线
#   ChatControlClient 只负责把"旧连接该下线了"推给目标实例
set(SERVER_SOURCES
    src/StatusServer.cpp        # 主入口：启动 gRPC 服务
    src/StatusServiceImpl.cpp   # gRPC 服务实现
    src/SessionRegistry.cpp     # 会话租约与 epoch
    src/InstanceRegistry.cpp    # 实例注册与选路
    src/ChatControlClient.cpp   # 到聊天实例的控制链路（踢人）
)

# ---------- 头文件清单 ----------
# 列出头文件便于 IDE 展示，实际的 #include 依赖由编译器在构建时追踪
set(SERVER_HEADERS
    include/StatusServiceImpl.h
    include/SessionRegistry.h
    include/InstanceRegistry.h
    include/ChatControlClient.h
    include/const.h
)

# ---------- 可执行目标 ----------
add_executable(${SERVER_NAME} ${SERVER_SOURCES} ${SERVER_HEADERS})

# 本服务 include/ 目录加入头文件搜索路径
target_include_directories(${SERVER_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ---------- 链接库 ----------
# yunjian_proto:            共享 proto 生成代码（静态库）
# gRPC::grpc++:          gRPC C++ 运行时
# protobuf::libprotobuf: protobuf 运行时
# Boost::filesystem:     路径处理
# Boost::headers:        仅头文件的 Boost 库
# hiredis::hiredis:      Redis 客户端
# JsonCpp::JsonCpp:      JSON 库
# Threads::Threads:      线程支持
target_link_libraries(${SERVER_NAME} PRIVATE
    yunjian_common
    yunjian_proto
    gRPC::grpc++
    protobuf::libprotobuf
    Boost::filesystem
    Boost::headers
    hiredis::hiredis
    JsonCpp::JsonCpp
    Threads::Threads
)
if(WIN32)
    # Windows 下网络编程需要 ws2_32（Winsock）库
    target_link_libraries(${SERVER_NAME} PRIVATE ws2_32)
endif()

# 输出到统一的 bin/<服务名>/ 目录
set_target_properties(${SERVER_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${SERVER_NAME}")

# 构建后部署：三平台均复制 config.ini，Windows 额外复制可识别的运行时 DLL
# TARGET_FILE_DIR 自动对应单配置目录或多配置的 Debug/Release 子目录
yunjian_deploy(${SERVER_NAME})
