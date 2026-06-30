# ======================================================
#
# LikesProgramConfig
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_CONFIG_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Config/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Config/*.h
)

file(GLOB_RECURSE LP_CONFIG_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_CONFIG_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramConfig SHARED ${LP_CONFIG_SOURCES})
    target_compile_definitions(LikesProgramConfig
        PUBLIC LIKESPROGRAM_CONFIG_SHARED
        PRIVATE LIKESPROGRAM_CONFIG_EXPORTS
    )
else()
    add_library(LikesProgramConfig STATIC ${LP_CONFIG_SOURCES})
endif()

add_library(LikesProgram::Config ALIAS LikesProgramConfig)

target_sources(LikesProgramConfig PRIVATE ${LP_CONFIG_HEADERS} ${LP_CONFIG_PRIVATE_HEADERS})

target_link_libraries(LikesProgramConfig PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramConfig
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

set_target_properties(LikesProgramConfig PROPERTIES
    EXPORT_NAME Config
    OUTPUT_NAME "LikesProgramConfig"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramConfig PROPERTIES
        OUTPUT_NAME "LikesProgramConfigStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramConfig PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramConfig PRIVATE /utf-8 /W4)
    endif()
else()
    target_compile_options(LikesProgramConfig PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramConfigTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ConfigPackageTests.cpp
    )
    target_link_libraries(LikesProgramConfigTests PRIVATE LikesProgram::Config)
    if (MSVC)
        target_compile_options(LikesProgramConfigTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramConfigTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramConfigTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramConfig>
                $<TARGET_FILE_DIR:LikesProgramConfigTests>
        )
    endif()

    add_test(NAME LikesProgramConfigTests COMMAND LikesProgramConfigTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramConfigExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/ConfigExample.cpp
    )
    target_link_libraries(LikesProgramConfigExample PRIVATE LikesProgram::Config)
    if (MSVC)
        target_compile_options(LikesProgramConfigExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramConfigExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramConfigExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramConfig>
                $<TARGET_FILE_DIR:LikesProgramConfigExample>
        )
    endif()

    add_executable(LikesProgramConfigBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ConfigBenchmark.cpp
    )
    target_link_libraries(LikesProgramConfigBenchmark PRIVATE LikesProgram::Config)
    if (MSVC)
        target_compile_options(LikesProgramConfigBenchmark PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramConfigBenchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramConfigBenchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramConfig>
                $<TARGET_FILE_DIR:LikesProgramConfigBenchmark>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramConfig
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
