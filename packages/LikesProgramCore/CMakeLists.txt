# ======================================================
#
# LikesProgramCore
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_CORE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Core/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Core/*.h
)

file(GLOB_RECURSE LP_CORE_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_CORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramCore SHARED ${LP_CORE_SOURCES})
    target_compile_definitions(LikesProgramCore
        PUBLIC LIKESPROGRAM_CORE_SHARED
        PRIVATE LIKESPROGRAM_CORE_EXPORTS
    )
else()
    add_library(LikesProgramCore STATIC ${LP_CORE_SOURCES})
endif()

add_library(LikesProgram::Core ALIAS LikesProgramCore)

target_sources(LikesProgramCore PRIVATE ${LP_CORE_HEADERS} ${LP_CORE_PRIVATE_HEADERS})

target_include_directories(LikesProgramCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

set_target_properties(LikesProgramCore PROPERTIES
    EXPORT_NAME Core
    OUTPUT_NAME "LikesProgramCore"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramCore PROPERTIES
        OUTPUT_NAME "LikesProgramCoreStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramCore PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramCore PRIVATE /utf-8 /W4)
    endif()
else()
    target_compile_options(LikesProgramCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramCoreTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/CoreTests.cpp
    )
    target_link_libraries(LikesProgramCoreTests PRIVATE LikesProgram::Core)
    target_include_directories(LikesProgramCoreTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    if (MSVC)
        target_compile_options(LikesProgramCoreTests PRIVATE /utf-8 /W4)
    endif()

    add_test(NAME LikesProgramCoreTests COMMAND LikesProgramCoreTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramCoreExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/CoreExample.cpp
    )
    target_link_libraries(LikesProgramCoreExample PRIVATE LikesProgram::Core)
    if (MSVC)
        target_compile_options(LikesProgramCoreExample PRIVATE /utf-8 /W4)
    endif()

    add_executable(LikesProgramCoreBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CoreBenchmark.cpp
    )
    target_link_libraries(LikesProgramCoreBenchmark PRIVATE LikesProgram::Core)
    target_include_directories(LikesProgramCoreBenchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    if (MSVC)
        target_compile_options(LikesProgramCoreBenchmark PRIVATE /utf-8 /W4)
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramCore
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
