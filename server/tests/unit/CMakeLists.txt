# ============================================================
#  单元测试 — 纯逻辑，不连服务、不读配置、不起线程
#
#  每个 .cpp 编成一个独立可执行文件，各自注册为一条 CTest 用例：
#    ctest -L unit            只跑这一层（秒级）
#    ctest -R validation     按名字过滤
#
#  被测代码直接以源文件或头文件方式参与编译，不复制实现：
#    RequestValidation.h / const.h   头文件里的 inline 函数
#    ResourceFrame.h                 帧编码与文件名约束
#    MsgNode.cpp                     发包组帧（协议层唯一需要编译的 .cpp）
#    MysqlSession.h                  取值转换（链接 yunjian_common）
#
#  新增一个测试：在本目录加 .cpp，用 yunjian_unit_test(<名字> <源文件...>) 注册。
# ============================================================

# 统一注册：可执行目标名带 yunjian_ 前缀，CTest 用例名带 unit. 前缀，
# 这样 ctest 列表里一眼能看出属于哪一层
function(yunjian_unit_test NAME)
    add_executable(yunjian_${NAME} ${ARGN})
    # 断言头在 tests/ 下，由 yunjian_test_support 提供 include 路径
    target_link_libraries(yunjian_${NAME} PRIVATE yunjian_test_support)
    add_test(NAME unit.${NAME} COMMAND yunjian_${NAME})
    set_tests_properties(unit.${NAME} PROPERTIES
        LABELS unit
        # 纯逻辑用例不该卡住；超时说明死循环或等锁
        TIMEOUT 60)
endfunction()

# ---------- 协议校验层 ----------
# 只依赖 const.h 与 jsoncpp；const.h 中的 Defer 是纯头文件，不需要链接公共库
yunjian_unit_test(request_validation
    test_request_validation.cpp)
target_include_directories(yunjian_request_validation PRIVATE
    "${PROJECT_SOURCE_DIR}/services/ChatServer/include"
    "${PROJECT_SOURCE_DIR}/common/include")
target_link_libraries(yunjian_request_validation PRIVATE
    JsonCpp::JsonCpp)

# ---------- 资源通道帧编码 ----------
yunjian_unit_test(resource_frame
    test_resource_frame.cpp)
target_include_directories(yunjian_resource_frame PRIVATE
    "${PROJECT_SOURCE_DIR}/services/ResourceServer/include")
target_link_libraries(yunjian_resource_frame PRIVATE
    JsonCpp::JsonCpp
    Boost::filesystem)

# ---------- 协议边界与取值转换 ----------
# MsgNode.cpp 直接参与编译：它是协议层唯一的实现文件，
# 复用到测试里才能保证测的就是服务端实际用的那份组包逻辑
yunjian_unit_test(protocol_limits
    test_protocol_limits.cpp
    "${PROJECT_SOURCE_DIR}/services/ChatServer/src/MsgNode.cpp")
target_include_directories(yunjian_protocol_limits PRIVATE
    "${PROJECT_SOURCE_DIR}/services/ChatServer/include")
# 取值转换在公共库里；Boost.Asio 由 yunjian_common 以 PUBLIC 传递
target_link_libraries(yunjian_protocol_limits PRIVATE
    yunjian_common)
