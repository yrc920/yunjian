# ============================================================
#  集成测试 — 真连本地栈的端到端用例
#
#  每个 .cpp 编成一个可执行文件，注册为一条 CTest 用例。测试程序
#  用同步 asio 直接说协议（帧格式、HTTP 登录），不依赖任何外部脚本。
#
#  前置条件：
#    · 网关 / ChatServer / ResourceServer / MySQL / Redis 已启动
#    · 测试账号在库中存在，且至少有一个单聊会话（洗库快照自带）
#  缺任一条时测试返回 77，CTest 记为 Skipped。
#
#  账号与地址通过命令行传入，可在 configure 时覆盖：
#    -DXIGU_TEST_GATE=127.0.0.1:8080 -DXIGU_TEST_EMAIL=... -DXIGU_TEST_PASSWD=...
#  passwd 是客户端 xor 之后的形态（123456 -> 745230），不是界面输入的明文。
#
#  互斥：三条用例共用同一个账号，并行跑会互相顶号（SessionFenced），
#  因此用 RESOURCE_LOCK 串行化——-j 只对单元测试生效。
# ============================================================

set(XIGU_TEST_GATE "127.0.0.1:8080" CACHE STRING "集成测试连接的 GateServer 地址")
set(XIGU_TEST_RESOURCE "127.0.0.1:9090" CACHE STRING "集成测试连接的 ResourceServer 地址")
set(XIGU_TEST_EMAIL "456@456.com" CACHE STRING "集成测试账号邮箱")
set(XIGU_TEST_PASSWD "745230" CACHE STRING "集成测试账号密码（客户端 xor 之后的值）")

# ---------- 客户端库 ----------
# 登录与帧收发只实现一次；三个用例都链接它
add_library(xigu_test_client STATIC
    TestClient.cpp
    TestClient.h)
target_include_directories(xigu_test_client PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(xigu_test_client PUBLIC
    Boost::headers
    JsonCpp::JsonCpp)

# ---------- 注册 ----------
# INCLUDE_DIR 是该用例要引用的服务端头文件目录：
#   ChatServer/include      聊天协议的消息 ID
#   ResourceServer/include  资源协议的消息 ID 与分片上限
function(xigu_integration_test NAME SOURCE INCLUDE_DIR)
    add_executable(xigu_${NAME} ${SOURCE})
    # 服务的 const.h 会引用 common 的 Defer.h，两个目录都要在搜索路径里
    target_include_directories(xigu_${NAME} PRIVATE
        "${INCLUDE_DIR}"
        "${PROJECT_SOURCE_DIR}/common/include")
    target_link_libraries(xigu_${NAME} PRIVATE
        xigu_test_client
        xigu_test_support)

    add_test(NAME integration.${NAME}
        COMMAND xigu_${NAME}
            --gate "${XIGU_TEST_GATE}"
            --res "${XIGU_TEST_RESOURCE}"
            --email "${XIGU_TEST_EMAIL}"
            --passwd "${XIGU_TEST_PASSWD}"
            # 测试上传的文件写在资源服务的 storage 下，结束时按这个目录清理
            --storage "$<TARGET_FILE_DIR:ResourceServer>/storage")

    set_tests_properties(integration.${NAME} PROPERTIES
        LABELS integration
        SKIP_RETURN_CODE 77
        # 卡住通常是服务端没回包，比默认无超时更容易定位
        TIMEOUT 120
        # 同一账号只能有一条活跃连接，三条用例必须串行
        RESOURCE_LOCK xigu_test_account)
endfunction()

xigu_integration_test(chat_flow test_chat_flow.cpp
    "${PROJECT_SOURCE_DIR}/services/ChatServer/include")
xigu_integration_test(group_flow test_group_flow.cpp
    "${PROJECT_SOURCE_DIR}/services/ChatServer/include")
xigu_integration_test(resource_flow test_resource_flow.cpp
    "${PROJECT_SOURCE_DIR}/services/ResourceServer/include")
# 资源用例结束时按 storage 目录清理自己上传的文件，需要 filesystem
target_link_libraries(xigu_resource_flow PRIVATE Boost::filesystem)
