cmake_minimum_required(VERSION 3.15)

# ======================================================
# tools/likesprogram-config-consumer-check/CMakeLists.txt
#
# Config 安装包外部消费方验收工程：
# 通过 find_package(LikesProgram) 验证 Config 安装导出可被独立工程消费。
# ======================================================

project(LikesProgramConfigConsumerCheck LANGUAGES CXX)

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
add_executable(LikesProgramConfigConsumerCheck main.cpp)
target_link_libraries(LikesProgramConfigConsumerCheck PRIVATE LikesProgram::Config)
set_target_properties(LikesProgramConfigConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-config-consumer-check")

# ======================================================
# 编译告警
# ======================================================
if (MSVC)
    target_compile_options(LikesProgramConfigConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramConfigConsumerCheck PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
