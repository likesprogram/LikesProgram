# ======================================================
#
# LikesProgramNet
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_NET_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Net/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Net/*.h
)

file(GLOB_RECURSE LP_NET_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_NET_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramNet SHARED ${LP_NET_SOURCES})
    target_compile_definitions(LikesProgramNet
        PUBLIC LIKESPROGRAM_NET_SHARED
        PRIVATE LIKESPROGRAM_NET_EXPORTS
    )
else()
    add_library(LikesProgramNet STATIC ${LP_NET_SOURCES})
endif()

add_library(LikesProgram::Net ALIAS LikesProgramNet)

target_sources(LikesProgramNet PRIVATE ${LP_NET_HEADERS} ${LP_NET_PRIVATE_HEADERS})

target_link_libraries(LikesProgramNet PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramNet
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

set_target_properties(LikesProgramNet PROPERTIES
    EXPORT_NAME Net
    OUTPUT_NAME "LikesProgramNet"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramNet PROPERTIES
        OUTPUT_NAME "LikesProgramNetStatic"
    )
endif()

# =========================
# 平台相关宏/系统库
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramNet PUBLIC NOMINMAX _WINSOCK_DEPRECATED_NO_WARNINGS)
    target_link_libraries(LikesProgramNet PUBLIC ws2_32)
    if (MSVC)
        target_compile_options(LikesProgramNet PRIVATE /utf-8 /W4)
    endif()
else()
    target_compile_options(LikesProgramNet PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramNetTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/NetPackageTests.cpp
    )
    target_link_libraries(LikesProgramNetTests PRIVATE LikesProgram::Net)
    if (MSVC)
        target_compile_options(LikesProgramNetTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramNetTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramNetTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:LikesProgramNetTests>
        )
    endif()

    add_test(NAME LikesProgramNetTests COMMAND LikesProgramNetTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramNetExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/NetExample.cpp
    )
    target_link_libraries(LikesProgramNetExample PRIVATE LikesProgram::Net)
    if (MSVC)
        target_compile_options(LikesProgramNetExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramNetExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramNetExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:LikesProgramNetExample>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramNet
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
