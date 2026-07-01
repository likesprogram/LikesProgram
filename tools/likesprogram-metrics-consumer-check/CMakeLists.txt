cmake_minimum_required(VERSION 3.15)

# ======================================================
# tools/likesprogram-metrics-consumer-check/CMakeLists.txt
#
# Metrics 安装包外部消费方验收工程：
# 通过 find_package(LikesProgram) 验证 Metrics 安装导出可被独立工程消费。
# ======================================================

project(LikesProgramMetricsConsumerCheck LANGUAGES CXX)

# ======================================================
# 设置 C++ 标准
# ======================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ======================================================
# 查找已安装的 LikesProgram
# ======================================================
find_package(LikesProgram CONFIG REQUIRED)

# ======================================================
# 定义外部消费方验收程序
# ======================================================
add_executable(LikesProgramMetricsConsumerCheck main.cpp)
target_link_libraries(LikesProgramMetricsConsumerCheck PRIVATE LikesProgram::Metrics)
set_target_properties(LikesProgramMetricsConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-metrics-consumer-check")

# ======================================================
# 编译告警
# ======================================================
if (MSVC)
    target_compile_options(LikesProgramMetricsConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramMetricsConsumerCheck PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
