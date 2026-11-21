cmake_minimum_required(VERSION 3.16)
project(cpptd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(CTP_ROOT "$ENV{HOME}/ctp_sdk" CACHE PATH "CTP SDK 根目录")
set(CTP_TRADER ${CTP_ROOT}/trader)
set(CTP_MD     ${CTP_ROOT}/md)

# CTP 的 .so 没有 lib 前缀，必须用 IMPORTED 目标指定完整路径
add_library(ctp_trader SHARED IMPORTED)
set_target_properties(ctp_trader PROPERTIES
    IMPORTED_LOCATION ${CTP_TRADER}/thosttraderapi_se.so)

add_library(ctp_md SHARED IMPORTED)
set_target_properties(ctp_md PROPERTIES
    IMPORTED_LOCATION ${CTP_MD}/thostmduserapi_se.so)

# 主程序：TD 主线程 + MD 线程同进程
add_executable(cpptd
    src/main.cpp
    src/log.cpp
    src/risk.cpp
    src/td_gateway.cpp
    src/md_feed.cpp
)
target_include_directories(cpptd PRIVATE include ${CTP_TRADER} ${CTP_MD})
target_link_libraries(cpptd PRIVATE ctp_trader ctp_md)

# 单元测试：不依赖 CTP SDK
add_executable(risk_test
    tests/risk_test.cpp
    src/risk.cpp
)
target_include_directories(risk_test PRIVATE include)

