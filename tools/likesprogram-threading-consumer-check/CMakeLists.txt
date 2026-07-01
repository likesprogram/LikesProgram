cmake_minimum_required(VERSION 3.15)

# ======================================================
# tools/likesprogram-threading-consumer-check/CMakeLists.txt
#
# Threading 安装包外部消费方验收工程：
# 通过 find_package(LikesProgram) 验证 Threading 安装导出可被独立工程消费。
# ======================================================

project(LikesProgramThreadingConsumerCheck LANGUAGES CXX)

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
add_executable(LikesProgramThreadingConsumerCheck main.cpp)
target_link_libraries(LikesProgramThreadingConsumerCheck PRIVATE LikesProgram::Threading)
set_target_properties(LikesProgramThreadingConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-threading-consumer-check")

# ======================================================
# 编译告警
# ======================================================
if (MSVC)
    target_compile_options(LikesProgramThreadingConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramThreadingConsumerCheck PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
