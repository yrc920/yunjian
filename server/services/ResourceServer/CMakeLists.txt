# ============================================================
#  ResourceServer — 头像/文件断点续传服务（boost::asio）
#
#  与 ChatServer 的差异：
#    · 帧头为 2 字节消息ID + 4 字节包体长度，容纳分片数据
#    · 逻辑线程按会话分派，落盘线程按文件名分派
#    · 连接先校验 uid + token，再绑定到本次传输
#
#  依赖: Boost + hiredis + jsoncpp + xiguchat_proto(gRPC) + Threads（MySQL 走 xiguchat_common）
#  产物: 单配置为 bin/ResourceServer/，多配置为 bin/ResourceServer/<配置名>/
# ============================================================

set(SERVER_NAME ResourceServer)

# 客户端连接的 TLS 与证书解析依赖 OpenSSL；静态链接时需要显式查找
find_package(OpenSSL REQUIRED)

# ---------- 源文件清单 ----------
set(SERVER_SOURCES
    src/ResourceServer.cpp     # 主入口：监听、信号与停服顺序
    src/StorageJanitor.cpp     # 残留 .part 的定期清理
    src/CServer.cpp            # TCP 监听器
    src/CSession.cpp           # 2+4 协议收发与连接清理
    src/MsgNode.cpp            # 头部与包体编码
    src/LogicSystem.cpp        # 逻辑线程池与续传进度
    src/LogicWorker.cpp        # 单个逻辑线程
    src/ResourceHandler.cpp    # 上传、同步、下载协议处理
    src/FileSystem.cpp         # 文件线程池
    src/FileWorker.cpp         # 分片落盘与头像入库
    src/DownloadWorker.cpp     # 分片读取
    src/MysqlDao.cpp           # 用户资料查询与头像更新
    src/ChatGrpcClient.cpp     # 图片上传完成后通知 ChatServer
    src/StatusGrpcClient.cpp   # 在线位置查询：接收方在哪台 ChatServer
)

set(SERVER_HEADERS
    include/CServer.h
    include/CSession.h
    include/MsgNode.h
    include/LogicSystem.h
    include/LogicWorker.h
    include/ResourceHandler.h
    include/ResourceFrame.h    # 帧编码与文件名约束（纯函数，单元测试直接覆盖）
    include/StorageJanitor.h   # 清理线程
    include/FileSystem.h
    include/FileWorker.h
    include/DownloadWorker.h
    include/FileTask.h
    include/MysqlDao.h
    include/ChatGrpcClient.h
    include/const.h
    include/data.h
)

# ---------- 可执行目标 ----------
add_executable(${SERVER_NAME} ${SERVER_SOURCES} ${SERVER_HEADERS})
target_include_directories(${SERVER_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${SERVER_NAME} PRIVATE
    # 公共基础设施（配置、事件循环线程池、Redis 连接池）
    xiguchat_common
    # 图片上传完成通知 ChatServer；xiguchat_proto 已 PUBLIC 链接 gRPC/protobuf
    xiguchat_proto
    Boost::filesystem
    Boost::headers
    hiredis::hiredis
    JsonCpp::JsonCpp
    protobuf::libprotobuf
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(WIN32)
    target_link_libraries(${SERVER_NAME} PRIVATE ws2_32)
endif()

set_target_properties(${SERVER_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${SERVER_NAME}")

xiguchat_deploy(${SERVER_NAME})
