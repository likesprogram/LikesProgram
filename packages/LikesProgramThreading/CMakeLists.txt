# ======================================================
#
# LikesProgramThreading
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_THREADING_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Threading/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Threading/*.h
)

file(GLOB_RECURSE LP_THREADING_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_THREADING_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramThreading SHARED ${LP_THREADING_SOURCES})
    target_compile_definitions(LikesProgramThreading
        PUBLIC LIKESPROGRAM_THREADING_SHARED
        PRIVATE LIKESPROGRAM_THREADING_EXPORTS
    )
else()
    add_library(LikesProgramThreading STATIC ${LP_THREADING_SOURCES})
endif()

add_library(LikesProgram::Threading ALIAS LikesProgramThreading)

target_sources(LikesProgramThreading PRIVATE ${LP_THREADING_HEADERS} ${LP_THREADING_PRIVATE_HEADERS})

target_link_libraries(LikesProgramThreading PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramThreading
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_compile_features(LikesProgramThreading PUBLIC cxx_std_20)

set_target_properties(LikesProgramThreading PROPERTIES
    EXPORT_NAME Threading
    OUTPUT_NAME "LikesProgramThreading"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramThreading PROPERTIES
        OUTPUT_NAME "LikesProgramThreadingStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramThreading PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramThreading
            PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            PRIVATE /W4
        )
    endif()
else()
    target_compile_options(LikesProgramThreading PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramThreadingTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ThreadingPackageTests.cpp
    )
    target_link_libraries(LikesProgramThreadingTests PRIVATE LikesProgram::Threading)
    if (MSVC)
        target_compile_options(LikesProgramThreadingTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramThreadingTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramThreadingTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramThreading>
                $<TARGET_FILE_DIR:LikesProgramThreadingTests>
        )
    endif()

    add_test(NAME LikesProgramThreadingTests COMMAND LikesProgramThreadingTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramThreadingExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/ThreadingExample.cpp
    )
    target_link_libraries(LikesProgramThreadingExample PRIVATE LikesProgram::Threading)
    if (MSVC)
        target_compile_options(LikesProgramThreadingExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramThreadingExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramThreadingExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramThreading>
                $<TARGET_FILE_DIR:LikesProgramThreadingExample>
        )
    endif()

    add_executable(LikesProgramThreadingBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ThreadingBenchmark.cpp
    )
    target_link_libraries(LikesProgramThreadingBenchmark PRIVATE LikesProgram::Threading)
    if (MSVC)
        target_compile_options(LikesProgramThreadingBenchmark PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramThreadingBenchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramThreadingBenchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramThreading>
                $<TARGET_FILE_DIR:LikesProgramThreadingBenchmark>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramThreading
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
