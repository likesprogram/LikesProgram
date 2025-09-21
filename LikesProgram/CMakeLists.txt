# ======================================================
#
# CMakeLists.txt
#
# 可选参数：
#   -DBUILD_SHARED_LIBS=ON      # 编译为动态库
#   -DENABLE_EXAMPLES=ON        # 构建示例
#   -DENABLE_STRICT_WARNINGS=ON # 启用严格警告
#
# ======================================================

cmake_minimum_required(VERSION 3.15)

project(LikesProgram
    VERSION 1.0.0
    LANGUAGES CXX
)

# =========================
# 配置选项
# =========================
option(BUILD_SHARED_LIBS "Build LikesProgram as shared library" OFF)
option(ENABLE_EXAMPLES   "Build example/demo programs"           ON)
option(ENABLE_STRICT_WARNINGS "Enable strict compiler warnings" OFF)

# =========================
# 设置 C++ 标准
# =========================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/*.h
)
file(GLOB_RECURSE LP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LikesProgram/*.cpp
)

# =========================
# 定义库目标
# =========================
add_library(LikesProgram ${LP_SOURCES})
target_sources(LikesProgram PRIVATE ${LP_HEADERS})

target_include_directories(LikesProgram
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ---------- 输出名和目录 ----------
if (BUILD_SHARED_LIBS)
    # 动态库
    set_target_properties(LikesProgram PROPERTIES
        OUTPUT_NAME "LikesProgram"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
else()
    # 静态库
    set_target_properties(LikesProgram PROPERTIES
        OUTPUT_NAME "LikesProgramStatic"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif()

# Debug 后缀
set_target_properties(LikesProgram PROPERTIES
    DEBUG_POSTFIX "d"
)

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgram
        PUBLIC
            _WINSOCK_DEPRECATED_NO_WARNINGS
            NOMINMAX
    )
    if (BUILD_SHARED_LIBS)
        target_compile_definitions(LikesProgram PRIVATE LIKESPROGRAM_EXPORT LIKESPROGRAM_LIB_EXPORTS)
    endif()
    if (MSVC)
        target_compile_options(LikesProgram PRIVATE /utf-8 /W4)
    endif()
elseif (APPLE)
    target_compile_definitions(LikesProgram PUBLIC LIKESPROGRAM_EXPORT)
    target_compile_options(LikesProgram PRIVATE -Wall -Wextra -Wpedantic)
elseif (UNIX)
    target_compile_definitions(LikesProgram PUBLIC LIKESPROGRAM_EXPORT)
    target_compile_options(LikesProgram PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 严格警告
# =========================
if (ENABLE_STRICT_WARNINGS)
    include(cmake/CompilerWarnings.cmake)
    enable_strict_warnings(LikesProgram)
endif()

# =========================
# 示例程序
# =========================
if (ENABLE_EXAMPLES)
    # 测试代码头文件
    file(GLOB_RECURSE TEST_HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/include/test/*.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/include/test/*.h
    )

    # Demo 可执行文件（把所有源文件都编进去）
    add_executable(LikesProgramDemo
        ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp
        ${LP_HEADERS}
        ${LP_SOURCES}
        ${TEST_HEADERS}
    )

    # 头文件搜索路径
    target_include_directories(LikesProgramDemo PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include/test
    )

    # MSVC 特定编译定义
    target_compile_definitions(LikesProgramDemo PRIVATE
        $<$<BOOL:MSVC>:_CRT_SECURE_NO_WARNINGS>
    )
endif()

# =========================
# 安装规则
# =========================
if (BUILD_SHARED_LIBS)
    install(TARGETS LikesProgram
        EXPORT LikesProgramTargets
        RUNTIME DESTINATION bin     # .dll
        LIBRARY DESTINATION lib     # .so/.dylib
        ARCHIVE DESTINATION lib     # .lib (import)
    )
else()
    install(TARGETS LikesProgram
        EXPORT LikesProgramTargets
        ARCHIVE DESTINATION lib     # 静态库
    )
endif()

install(DIRECTORY include/LikesProgram DESTINATION include)

# =========================
# 生成并安装 CMake 包配置文件
# =========================
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in")
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfig.cmake"
        INSTALL_DESTINATION lib/cmake/LikesProgram
    )
    install(EXPORT LikesProgramTargets
        FILE LikesProgramTargets.cmake
        NAMESPACE LikesProgram::
        DESTINATION lib/cmake/LikesProgram
    )
endif()

# =========================
# 别名目标
# =========================
if(NOT TARGET LikesProgram::LikesProgram)
    add_library(LikesProgram::LikesProgram ALIAS LikesProgram)
endif()
