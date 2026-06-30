# ======================================================
#
# tools/likesprogram-doctor/CMakeLists.txt
#
# 发布诊断工具：
#   构建 likesprogram-doctor 命令行诊断程序。
#   支持随顶层工程构建，也支持安装包外部独立构建。
#
# 顶层工程可选参数：
#   -DLIKESPROGRAM_BUILD_TOOLS=ON
#   -DLIKESPROGRAM_BUILD_TESTS=ON
#
# 外部独立构建可选参数：
#   -DCMAKE_PREFIX_PATH=<LikesProgram 安装目录>
#   -DBUILD_TESTING=ON
#
# ======================================================

cmake_minimum_required(VERSION 3.15)

project(LikesProgramDoctor LANGUAGES CXX)

# =========================
# 解析依赖目标
# =========================
if (NOT TARGET LikesProgram::Core)
    find_package(LikesProgram CONFIG REQUIRED)
endif()

# =========================
# 定义工具目标
# =========================
add_executable(LikesProgramDoctor main.cpp)

target_compile_features(LikesProgramDoctor PRIVATE cxx_std_20)
target_link_libraries(LikesProgramDoctor PRIVATE LikesProgram::Core)

if (TARGET LikesProgram::Logging)
    target_link_libraries(LikesProgramDoctor PRIVATE LikesProgram::Logging)
    target_compile_definitions(LikesProgramDoctor PRIVATE LIKESPROGRAM_DOCTOR_HAS_LOGGING=1)
else()
    target_compile_definitions(LikesProgramDoctor PRIVATE LIKESPROGRAM_DOCTOR_HAS_LOGGING=0)
endif()

if (TARGET LikesProgram::Config)
    target_link_libraries(LikesProgramDoctor PRIVATE LikesProgram::Config)
    target_compile_definitions(LikesProgramDoctor PRIVATE LIKESPROGRAM_DOCTOR_HAS_CONFIG=1)
else()
    target_compile_definitions(LikesProgramDoctor PRIVATE LIKESPROGRAM_DOCTOR_HAS_CONFIG=0)
endif()

# =========================
# 输出与编译策略
# =========================
set_target_properties(LikesProgramDoctor PROPERTIES
    OUTPUT_NAME "likesprogram-doctor"
)

if (WIN32)
    target_compile_definitions(LikesProgramDoctor PRIVATE NOMINMAX)
endif()

if (MSVC)
    target_compile_options(LikesProgramDoctor PRIVATE /utf-8 /W4)
else()
    target_compile_options(LikesProgramDoctor PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# Windows 运行时辅助复制
# =========================
function(likesprogram_doctor_copy_runtime localTarget importedTarget)
    if (TARGET ${localTarget})
        set(runtimeTarget ${localTarget}) # 源码树构建时使用真实目标
    elseif (TARGET ${importedTarget})
        set(runtimeTarget ${importedTarget}) # 安装后外部构建时使用 imported 目标
    else()
        return()
    endif()

    add_custom_command(TARGET LikesProgramDoctor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:${runtimeTarget}>
            $<TARGET_FILE_DIR:LikesProgramDoctor>
    )
endfunction()

if (WIN32)
    likesprogram_doctor_copy_runtime(LikesProgramCore LikesProgram::Core)
    likesprogram_doctor_copy_runtime(LikesProgramLogging LikesProgram::Logging)
    likesprogram_doctor_copy_runtime(LikesProgramConfig LikesProgram::Config)
endif()

# =========================
# 测试与安装
# =========================
if ((DEFINED LIKESPROGRAM_BUILD_TESTS AND LIKESPROGRAM_BUILD_TESTS)
    OR (DEFINED BUILD_TESTING AND BUILD_TESTING))
    add_test(NAME LikesProgramDoctorSmoke COMMAND LikesProgramDoctor --format text)
endif()

install(TARGETS LikesProgramDoctor
    RUNTIME DESTINATION bin
)

install(FILES
    "${CMAKE_CURRENT_LIST_DIR}/../README.md"
    DESTINATION share/doc/LikesProgram/tools
)
