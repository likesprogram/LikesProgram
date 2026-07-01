# ======================================================
#
# CMakeLists.txt
#
# 可选参数：
#   -DLIKESPROGRAM_BUILD_SHARED=ON
#   -DLIKESPROGRAM_BUILD_TESTS=ON
#   -DLIKESPROGRAM_BUILD_EXAMPLES=ON
#   -DLIKESPROGRAM_BUILD_TOOLS=ON
#   -DLIKESPROGRAM_BUILD_LOGGING=OFF
#   -DLIKESPROGRAM_BUILD_CONFIG=OFF
#   -DLIKESPROGRAM_BUILD_METRICS=OFF
#   -DLIKESPROGRAM_BUILD_THREADING=OFF
#   -DLIKESPROGRAM_BUILD_NET=OFF
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
option(LIKESPROGRAM_BUILD_SHARED "Build shared libraries" ON)
option(LIKESPROGRAM_BUILD_TESTS "Build tests" ON)
option(LIKESPROGRAM_BUILD_EXAMPLES "Build examples" ON)
option(LIKESPROGRAM_BUILD_TOOLS "Build production command-line tools" ON)
option(LIKESPROGRAM_BUILD_LOGGING "Build LikesProgramLogging" OFF)
option(LIKESPROGRAM_BUILD_CONFIG "Build LikesProgramConfig" OFF)
option(LIKESPROGRAM_BUILD_METRICS "Build LikesProgramMetrics" OFF)
option(LIKESPROGRAM_BUILD_THREADING "Build LikesProgramThreading" OFF)
option(LIKESPROGRAM_BUILD_NET "Build LikesProgramNet" OFF)
option(LIKESPROGRAM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(LIKESPROGRAM_ENABLE_MSVC_CODE_ANALYSIS "Enable MSVC code analysis" ON)

# =========================
# 设置 C++ 标准
# =========================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# =========================
# 严格编译/审查策略
# =========================
if (MSVC)
    add_compile_options(/utf-8 /W4)
    if (LIKESPROGRAM_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
    if (LIKESPROGRAM_ENABLE_MSVC_CODE_ANALYSIS)
        add_compile_options(/analyze)
    endif()
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if (LIKESPROGRAM_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# =========================
# 启用测试
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    enable_testing()
endif()

# =========================
# 包含组件
# =========================
add_subdirectory(packages/LikesProgramCore)

function(likesprogram_add_optional_package optionName packageName)
    if (${optionName})
        set(packageDir "${CMAKE_CURRENT_SOURCE_DIR}/packages/${packageName}") # 可选包源码目录，未迁移前允许缺席
        if (EXISTS "${packageDir}/CMakeLists.txt")
            add_subdirectory("packages/${packageName}")
        else()
            message(FATAL_ERROR "${packageName} has not been migrated yet. Please set ${optionName}=OFF.")
        endif()
    endif()
endfunction()

likesprogram_add_optional_package(LIKESPROGRAM_BUILD_LOGGING LikesProgramLogging)
likesprogram_add_optional_package(LIKESPROGRAM_BUILD_CONFIG LikesProgramConfig)
likesprogram_add_optional_package(LIKESPROGRAM_BUILD_METRICS LikesProgramMetrics)
likesprogram_add_optional_package(LIKESPROGRAM_BUILD_THREADING LikesProgramThreading)
likesprogram_add_optional_package(LIKESPROGRAM_BUILD_NET LikesProgramNet)

# =========================
# 发布工具
# =========================
if (LIKESPROGRAM_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# =========================
# 生成并安装 CMake 包配置文件
# =========================
include(CMakePackageConfigHelpers)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LikesProgramConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfig.cmake"
    INSTALL_DESTINATION lib/cmake/LikesProgram
)

install(EXPORT LikesProgramTargets
    FILE LikesProgramTargets.cmake
    NAMESPACE LikesProgram::
    DESTINATION lib/cmake/LikesProgram
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/LikesProgramConfigVersion.cmake"
    DESTINATION lib/cmake/LikesProgram
)
