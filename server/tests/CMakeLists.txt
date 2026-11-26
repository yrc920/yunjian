# ============================================================
#  测试 — CTest 驱动，分两层
#
#  CTest 是什么：它不是断言框架，只做三件事——按名字启动一个命令、
#  看进程退出码、把结果汇总成一张表。退出码 0 记为通过，非 0 记为失败，
#  77 记为 Skipped。所以「怎么判对错」写在测试程序里，CTest 只负责
#  把它们串起来、给你一个总账和一个总的退出码。
#
#  用法（在 server/ 下）：
#    ctest --preset macos-clang             跑当前预设的全部用例
#    ctest --preset macos-clang -L unit     只跑单元测试：秒级，不需要起服务
#    ctest --preset macos-clang -R resource 按名字过滤
#    ctest --preset macos-clang -j 4        并行（集成测试有互斥锁，不会互相顶号）
#    ctest --preset macos-clang --rerun-failed  只重跑上次失败的
#
#  两层分工：
#    unit         纯逻辑，不连网络、不读 config.ini，任何机器随时可跑；
#                 改协议校验、帧编码、包长计算后先跑这一层
#    integration  真连本地栈（网关 + ChatServer + ResourceServer + MySQL + Redis）；
#                 端口不通时返回 77，CTest 记为 Skipped 而不是失败，
#                 「忘记启动服务」和「服务有 bug」在报告里能区分开
#
#  关闭测试：configure 时加 -DBUILD_TESTING=OFF，部署构建不该带测试目标。
#  注意：在已有构建目录里切换这个开关，CMake 不会删掉早先生成的
#  CTestTestfile.cmake，ctest 仍会按旧文件跑——要彻底干净得从空目录配置。
# ============================================================

# 断言与用例注册头，单元测试和集成测试共用
add_library(yunjian_test_support INTERFACE)
target_include_directories(yunjian_test_support INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(unit)
add_subdirectory(integration)
