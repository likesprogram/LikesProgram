# ======================================================
#
# LikesProgramHttp
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_HTTP_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Http/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Http/*.h
)

file(GLOB_RECURSE LP_HTTP_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_HTTP_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramHttp SHARED ${LP_HTTP_SOURCES})
    target_compile_definitions(LikesProgramHttp
        PUBLIC LIKESPROGRAM_HTTP_SHARED
        PRIVATE LIKESPROGRAM_HTTP_EXPORTS
    )
else()
    add_library(LikesProgramHttp STATIC ${LP_HTTP_SOURCES})
endif()

add_library(LikesProgram::Http ALIAS LikesProgramHttp)

target_sources(LikesProgramHttp PRIVATE ${LP_HTTP_HEADERS} ${LP_HTTP_PRIVATE_HEADERS})

target_link_libraries(LikesProgramHttp PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramHttp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_compile_features(LikesProgramHttp PUBLIC cxx_std_20)

set_target_properties(LikesProgramHttp PROPERTIES
    EXPORT_NAME Http
    OUTPUT_NAME "LikesProgramHttp"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramHttp PROPERTIES
        OUTPUT_NAME "LikesProgramHttpStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramHttp PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramHttp
            PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            PRIVATE /W4
        )
    endif()
else()
    target_compile_options(LikesProgramHttp PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramHttpTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/HttpPackageTests.cpp
    )
    target_link_libraries(LikesProgramHttpTests PRIVATE LikesProgram::Http)
    if (MSVC)
        target_compile_options(LikesProgramHttpTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramHttpTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramHttpTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramHttp>
                $<TARGET_FILE_DIR:LikesProgramHttpTests>
        )
    endif()

    add_test(NAME LikesProgramHttpTests COMMAND LikesProgramHttpTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramHttpExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/HttpExample.cpp
    )
    target_link_libraries(LikesProgramHttpExample PRIVATE LikesProgram::Http)
    if (MSVC)
        target_compile_options(LikesProgramHttpExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramHttpExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramHttpExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramHttp>
                $<TARGET_FILE_DIR:LikesProgramHttpExample>
        )
    endif()

    add_executable(LikesProgramHttpBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/HttpBenchmark.cpp
    )
    target_link_libraries(LikesProgramHttpBenchmark PRIVATE LikesProgram::Http)
    if (MSVC)
        target_compile_options(LikesProgramHttpBenchmark PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramHttpBenchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramHttpBenchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramHttp>
                $<TARGET_FILE_DIR:LikesProgramHttpBenchmark>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramHttp
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
