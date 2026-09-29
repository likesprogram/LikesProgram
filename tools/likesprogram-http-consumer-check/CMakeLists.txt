cmake_minimum_required(VERSION 3.15)

# ======================================================
# tools/likesprogram-http-consumer-check/CMakeLists.txt
#
# 安装包外部消费方验收工程：
# 通过 find_package(LikesProgram) 验证 Http 安装导出可被独立工程消费。
# ======================================================

project(LikesProgramHttpConsumerCheck LANGUAGES CXX)

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
add_executable(LikesProgramHttpConsumerCheck main.cpp)
target_link_libraries(LikesProgramHttpConsumerCheck PRIVATE LikesProgram::Http)
set_target_properties(LikesProgramHttpConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-http-consumer-check")

# ======================================================
# 编译告警
# ======================================================
if (MSVC)
    target_compile_options(LikesProgramHttpConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramHttpConsumerCheck PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
