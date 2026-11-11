# ============================================================
#  ResourceServer — 头像/文件断点续传服务（boost::asio）
#
#  与 ChatServer 的差异：
#    · 帧头为 2 字节消息ID + 4 字节包体长度，容纳分片数据
#    · 逻辑线程按会话分派，落盘线程按文件名分派
#    · 连接先校验 uid + token，再绑定到本次传输
#
#  依赖: Boost + hiredis + jsoncpp + mysql-connector-cpp + xigu_proto(gRPC) + Threads
#  产物: 单配置为 bin/ResourceServer/，多配置为 bin/ResourceServer/<配置名>/
# ============================================================

set(SERVER_NAME ResourceServer)

# X DevAPI 压缩与 zlib 符号需要显式查找；vcpkg 提供 ZLIBConfig
find_package(ZLIB REQUIRED)
# X DevAPI 的 TLS 连接直接依赖 OpenSSL，静态链接时需要补上
find_package(OpenSSL REQUIRED)

# ---------- 源文件清单 ----------
set(SERVER_SOURCES
    src/ResourceServer.cpp     # 主入口：监听、信号与停服顺序
    src/CServer.cpp            # TCP 监听器
    src/CSession.cpp           # 2+4 协议收发与连接清理
    src/MsgNode.cpp            # 头部与包体编码
    src/LogicSystem.cpp        # 逻辑线程池与续传进度
    src/LogicWorker.cpp        # 单个逻辑线程
    src/ResourceHandler.cpp    # 上传、同步、下载协议处理
    src/FileSystem.cpp         # 文件线程池
    src/FileWorker.cpp         # 分片落盘与头像入库
    src/DownloadWorker.cpp     # 分片读取
    src/Base64.cpp             # 分片编解码
    src/ConfigMgr.cpp          # config.ini 读取
    src/IOContextPool.cpp      # io_context 线程池
    src/RedisMgr.cpp           # Redis 连接池
    src/MysqlMgr.cpp           # MySQL 管理入口
    src/MysqlDao.cpp           # 用户资料查询与头像更新
    src/ChatGrpcClient.cpp     # 图片上传完成后通知 ChatServer
)

set(SERVER_HEADERS
    include/CServer.h
    include/CSession.h
    include/MsgNode.h
    include/LogicSystem.h
    include/LogicWorker.h
    include/ResourceHandler.h
    include/FileSystem.h
    include/FileWorker.h
    include/DownloadWorker.h
    include/FileTask.h
    include/Base64.h
    include/ConfigMgr.h
    include/IOContextPool.h
    include/RedisMgr.h
    include/MysqlMgr.h
    include/MysqlDao.h
    include/ChatGrpcClient.h
    include/Singleton.h
    include/const.h
    include/data.h
)

# ---------- 可执行目标 ----------
add_executable(${SERVER_NAME} ${SERVER_SOURCES} ${SERVER_HEADERS})
target_include_directories(${SERVER_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${SERVER_NAME} PRIVATE
    # 图片上传完成通知 ChatServer；xigu_proto 已 PUBLIC 链接 gRPC/protobuf
    xigu_proto
    Boost::filesystem
    Boost::headers
    hiredis::hiredis
    JsonCpp::JsonCpp
    unofficial::mysql-connector-cpp::connector
    # mysql-connector-cpp 的 X DevAPI 静态库依赖 protobuf 与 zlib，但 unofficial
    # 配置不会传递它们；不补链接时会出现大量 MessageLite/ZeroCopyStream/符号缺失
    protobuf::libprotobuf
    ZLIB::ZLIB
    OpenSSL::SSL
    OpenSSL::Crypto
    Threads::Threads
)
if(APPLE)
    # X DevAPI 的 SRV 解析调用 res_9_*，macOS 需要显式链接 libresolv
    target_link_libraries(${SERVER_NAME} PRIVATE resolv)
endif()
if(WIN32)
    target_link_libraries(${SERVER_NAME} PRIVATE ws2_32)
endif()

set_target_properties(${SERVER_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${SERVER_NAME}")

xiguchat_deploy(${SERVER_NAME})
