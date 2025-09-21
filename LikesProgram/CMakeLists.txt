# ======================================================
#
# CMakeLists.txt
# 
# 可选参数：
#   编译为动态库：-DBUILD_SHARED_LIBS=ON
#   构建示例：-DENABLE_EXAMPLES=ON
#   启用严格警告：-DENABLE_STRICT_WARNINGS=ON
# ======================================================

# 指定 CMake 最低版本要求
cmake_minimum_required(VERSION 3.15)

# 定义项目
project(LikesProgram
    VERSION 1.0.0         # 项目版本号
    LANGUAGES CXX         # 使用的语言
)

# =========================
#  配置选项
# =========================
# 是否编译为共享库（ON = 动态库，OFF = 静态库）
option(BUILD_SHARED_LIBS "Build LikesProgram as shared library" OFF)
# 是否构建示例/演示程序
option(ENABLE_EXAMPLES  "Build example/demo programs"           ON)
# 是否启用严格编译警告
option(ENABLE_STRICT_WARNINGS "Enable strict compiler warnings" ON)

# =========================
#  设置 C++ 标准
# =========================
set(CMAKE_CXX_STANDARD 20)          # 要求 C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON) # 必须满足该标准
set(CMAKE_CXX_EXTENSIONS OFF)       # 禁止使用编译器特有扩展（比如 GNU++）

# =========================
#  收集源文件和头文件
# =========================
# 递归收集头文件（hpp / h）
file(GLOB_RECURSE LP_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/*.h
)

# 递归收集源码（cpp）
file(GLOB_RECURSE LP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LikesProgram/*.cpp
)

# =========================
#  定义库目标
# =========================
# 根据 BUILD_SHARED_LIBS 自动生成 SHARED 或 STATIC 库
add_library(LikesProgram ${LP_SOURCES})

# 为 IDE 展示头文件（不影响实际编译）
target_sources(LikesProgram PRIVATE ${LP_HEADERS})

# 配置 include 路径
target_include_directories(LikesProgram
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>  # 构建时使用
        $<INSTALL_INTERFACE:include>                           # 安装后使用
)

# =========================
#  平台相关宏/编译选项
# =========================
if (WIN32) # Windows 平台
    target_compile_definitions(LikesProgram
        PUBLIC
            _WINSOCK_DEPRECATED_NO_WARNINGS   # 屏蔽 WinSock 旧 API 警告
            NOMINMAX                          # 禁止 Windows 定义 min/max 宏
            LIKESPROGRAM_EXPORT               # 库的导出宏（见 LikesProgramLibExport.hpp）
    )

    # 如果构建动态库，则定义 LIKESPROGRAM_LIB_EXPORTS
    if (BUILD_SHARED_LIBS)
        target_compile_definitions(LikesProgram PRIVATE LIKESPROGRAM_LIB_EXPORTS)
    endif()

    # MSVC 专用编译选项：UTF-8 编码 + 最高级别警告
    if (MSVC)
        target_compile_options(LikesProgram PRIVATE /utf-8 /W4)
    endif()

elseif (APPLE) # macOS / iOS
    target_compile_definitions(LikesProgram PUBLIC LIKESPROGRAM_EXPORT)
    target_compile_options(LikesProgram PRIVATE -Wall -Wextra -Wpedantic)

elseif (UNIX) # Linux / *BSD / 其他类 Unix
    target_compile_definitions(LikesProgram PUBLIC LIKESPROGRAM_EXPORT)
    target_compile_options(LikesProgram PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 可选启用严格警告
# =========================
if(ENABLE_STRICT_WARNINGS)
    include(cmake/CompilerWarnings.cmake)
    enable_strict_warnings(LikesProgram)
endif()

# =========================
#  构建示例（可选）
# =========================
if (ENABLE_EXAMPLES)
    # 创建示例可执行文件
    add_executable(LikesProgramDemo src/main.cpp)

    # 链接库
    target_link_libraries(LikesProgramDemo PRIVATE LikesProgram)

    # 添加头文件目录
    target_include_directories(LikesProgramDemo PRIVATE include)

    # 给 Demo 设置 MSVC 特定宏，避免 CRT 警告
    target_compile_definitions(LikesProgramDemo PRIVATE
        $<$<BOOL:MSVC>:_CRT_SECURE_NO_WARNINGS>
    )
endif()

# =========================
#  安装规则
# =========================
# 安装库文件（静态库/动态库/可执行文件）
install(TARGETS LikesProgram
    EXPORT LikesProgramTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

# 安装头文件（保持 include/LikesProgram 层级）
install(DIRECTORY include/LikesProgram DESTINATION include)

# =========================
#  生成并安装 CMake 包配置文件
# =========================
include(CMakePackageConfigHelpers)

# 生成版本文件（用于 find_package 版本检查）
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# 如果存在 Config.cmake.in，则生成配置文件
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in")
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/Config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfig.cmake"
        INSTALL_DESTINATION lib/cmake/LikesProgram
    )

    # 安装导出信息（Targets）
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
