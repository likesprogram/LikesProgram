# ======================================================
#
# LikesProgramLogging
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_LOGGING_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Logging/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Logging/*.h
)

file(GLOB_RECURSE LP_LOGGING_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_LOGGING_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramLogging SHARED ${LP_LOGGING_SOURCES})
    target_compile_definitions(LikesProgramLogging
        PUBLIC LIKESPROGRAM_LOGGING_SHARED
        PRIVATE LIKESPROGRAM_LOGGING_EXPORTS
    )
else()
    add_library(LikesProgramLogging STATIC ${LP_LOGGING_SOURCES})
endif()

add_library(LikesProgram::Logging ALIAS LikesProgramLogging)

target_sources(LikesProgramLogging PRIVATE ${LP_LOGGING_HEADERS} ${LP_LOGGING_PRIVATE_HEADERS})

target_link_libraries(LikesProgramLogging PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramLogging
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

set_target_properties(LikesProgramLogging PROPERTIES
    EXPORT_NAME Logging
    OUTPUT_NAME "LikesProgramLogging"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramLogging PROPERTIES
        OUTPUT_NAME "LikesProgramLoggingStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramLogging PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramLogging PRIVATE /utf-8 /W4)
    endif()
else()
    target_compile_options(LikesProgramLogging PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramLoggingTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/LoggingPackageTests.cpp
    )
    target_link_libraries(LikesProgramLoggingTests PRIVATE LikesProgram::Logging)
    if (MSVC)
        target_compile_options(LikesProgramLoggingTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramLoggingTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramLoggingTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramLogging>
                $<TARGET_FILE_DIR:LikesProgramLoggingTests>
        )
    endif()

    add_test(NAME LikesProgramLoggingTests COMMAND LikesProgramLoggingTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramLoggingExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/LoggingExample.cpp
    )
    target_link_libraries(LikesProgramLoggingExample PRIVATE LikesProgram::Logging)
    if (MSVC)
        target_compile_options(LikesProgramLoggingExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramLoggingExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramLoggingExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramLogging>
                $<TARGET_FILE_DIR:LikesProgramLoggingExample>
        )
    endif()

    add_executable(LikesProgramLoggingBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/LoggingBenchmark.cpp
    )
    target_link_libraries(LikesProgramLoggingBenchmark PRIVATE LikesProgram::Logging)
    if (MSVC)
        target_compile_options(LikesProgramLoggingBenchmark PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramLoggingBenchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramLoggingBenchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramLogging>
                $<TARGET_FILE_DIR:LikesProgramLoggingBenchmark>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramLogging
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
