# ======================================================
#
# LikesProgramMetrics
#
# ======================================================

# =========================
# 收集源文件和头文件
# =========================
file(GLOB_RECURSE LP_METRICS_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Metrics/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Metrics/*.h
)

file(GLOB_RECURSE LP_METRICS_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_METRICS_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

# =========================
# 定义库目标
# =========================
if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramMetrics SHARED ${LP_METRICS_SOURCES})
    target_compile_definitions(LikesProgramMetrics
        PUBLIC LIKESPROGRAM_METRICS_SHARED
        PRIVATE LIKESPROGRAM_METRICS_EXPORTS
    )
else()
    add_library(LikesProgramMetrics STATIC ${LP_METRICS_SOURCES})
endif()

add_library(LikesProgram::Metrics ALIAS LikesProgramMetrics)

target_sources(LikesProgramMetrics PRIVATE ${LP_METRICS_HEADERS} ${LP_METRICS_PRIVATE_HEADERS})

target_link_libraries(LikesProgramMetrics PUBLIC LikesProgram::Core)

target_include_directories(LikesProgramMetrics
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

target_compile_features(LikesProgramMetrics PUBLIC cxx_std_20)

set_target_properties(LikesProgramMetrics PROPERTIES
    EXPORT_NAME Metrics
    OUTPUT_NAME "LikesProgramMetrics"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramMetrics PROPERTIES
        OUTPUT_NAME "LikesProgramMetricsStatic"
    )
endif()

# =========================
# 平台相关宏/编译选项
# =========================
if (WIN32)
    target_compile_definitions(LikesProgramMetrics PUBLIC NOMINMAX)
    if (MSVC)
        target_compile_options(LikesProgramMetrics
            PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/utf-8>
            PRIVATE /W4
        )
    endif()
else()
    target_compile_options(LikesProgramMetrics PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    set(LP_METRICS_TEST_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/MetricsPackageTests.cpp
    )
    if (LIKESPROGRAM_BUILD_SHARED)
        list(APPEND LP_METRICS_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/PercentileSketch.cpp)
    endif()

    add_executable(LikesProgramMetricsTests ${LP_METRICS_TEST_SOURCES})
    target_link_libraries(LikesProgramMetricsTests PRIVATE LikesProgram::Metrics)
    target_include_directories(LikesProgramMetricsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/include)
    if (MSVC)
        target_compile_options(LikesProgramMetricsTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramMetricsTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramMetricsTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramMetrics>
                $<TARGET_FILE_DIR:LikesProgramMetricsTests>
        )
    endif()

    add_test(NAME LikesProgramMetricsTests COMMAND LikesProgramMetricsTests)
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(LikesProgramMetricsExample
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/MetricsExample.cpp
    )
    target_link_libraries(LikesProgramMetricsExample PRIVATE LikesProgram::Metrics)
    if (MSVC)
        target_compile_options(LikesProgramMetricsExample PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramMetricsExample POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramMetricsExample>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramMetrics>
                $<TARGET_FILE_DIR:LikesProgramMetricsExample>
        )
    endif()

    add_executable(LikesProgramMetricsBenchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/MetricsBenchmark.cpp
    )
    target_link_libraries(LikesProgramMetricsBenchmark PRIVATE LikesProgram::Metrics)
    if (MSVC)
        target_compile_options(LikesProgramMetricsBenchmark PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramMetricsBenchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramMetricsBenchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramMetrics>
                $<TARGET_FILE_DIR:LikesProgramMetricsBenchmark>
        )
    endif()
endif()

# =========================
# 安装规则
# =========================
install(TARGETS LikesProgramMetrics
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
