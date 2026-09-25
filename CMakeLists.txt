cmake_minimum_required(VERSION 3.16)
project(PrintPay VERSION 1.0 LANGUAGES CXX)

# =============================================================================
# 打印计费助手 PrintPay —— 构建脚本
#   结构：printpay_core（核心逻辑，静态库，无界面）
#         PrintPay（图形界面主程序）
#         printpay_core_test（控制台自测，不需要界面）
#   依赖：Qt 6（Core / Widgets / Sql）+ Windows 打印后台 API（winspool）
# =============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)   # 核心库里的 JobFlow、界面类都带 Q_OBJECT，必须由 moc 生成元对象代码

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql)
if(MSVC)
    add_compile_options(/utf-8)   # 源码是 UTF-8，MSVC 下不指定会按 GBK 解析导致中文乱码
endif()

# ---------------------------------------------------------------- 核心库
# 打印监控、计费、配置、账号与流水记录：全部与界面解耦，方便单独测试
add_library(printpay_core STATIC
    src/core/spooler.cpp
    src/core/settings.cpp
    src/core/billing.cpp
    src/core/store.cpp
    src/core/job_flow.cpp
)
target_include_directories(printpay_core PUBLIC src)
target_link_libraries(printpay_core PUBLIC Qt6::Core Qt6::Sql winspool)

# ---------------------------------------------------------------- 图形界面主程序
add_executable(PrintPay WIN32
    src/main.cpp
    src/ui/main_window.cpp
    src/ui/login_dialog.cpp
    src/ui/approval_dialog.cpp
    src/ui/payment_dialog.cpp
    src/ui/settings_dialog.cpp
)
target_link_libraries(PrintPay PRIVATE printpay_core Qt6::Widgets)
if(WIN32)
    # shell32：主窗口里“以管理员身份重启”用的 ShellExecuteW("runas")
    target_link_libraries(PrintPay PRIVATE shell32)
endif()

# ---------------------------------------------------------------- 控制台自测
# 覆盖计费算法、配置读写、账号登录、流水记录，并可对真实打印队列做集成测试
add_executable(printpay_core_test tests/core_test.cpp)
target_link_libraries(printpay_core_test PRIVATE printpay_core)
# 自测程序自己没有 Q_OBJECT 类，关掉 AUTOMOC（否则会多出一个没必要的 moc 步骤）
set_target_properties(printpay_core_test PROPERTIES AUTOMOC OFF)

# ---------------------------------------------------------------- 资源文件拷贝
# 收款码图片与默认配置放到可执行文件旁边，方便直接替换
add_custom_command(TARGET PrintPay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:PrintPay>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.ini" "$<TARGET_FILE_DIR:PrintPay>/config.ini"
)
