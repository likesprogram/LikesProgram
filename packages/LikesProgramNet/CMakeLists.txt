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

# 仅把当前目标平台的私有 completion 实现加入构建，避免跨平台头/库泄漏。
list(FILTER LP_NET_SOURCES EXCLUDE REGEX "/src/platform/")

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB_RECURSE LP_NET_POSIX_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/posix/*.cpp
    )
    file(GLOB_RECURSE LP_NET_PLATFORM_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux/*.cpp
    )
    list(APPEND LP_NET_SOURCES ${LP_NET_POSIX_SOURCES} ${LP_NET_PLATFORM_SOURCES})
elseif (WIN32)
    file(GLOB_RECURSE LP_NET_PLATFORM_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/*.cpp
    )
    list(APPEND LP_NET_SOURCES ${LP_NET_PLATFORM_SOURCES})
else()
    message(FATAL_ERROR
        "LikesProgramNet supports only Linux completion or Windows IOCP backends; "
        "other platform backends follow the approved migration plans")
endif()

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
if (MSVC)
    target_compile_options(LikesProgramNet PRIVATE /utf-8 /W4)
else()
    target_compile_options(LikesProgramNet PRIVATE -Wall -Wextra -Wpedantic)
endif()

# =========================
# Linux io_uring completion I/O
# =========================
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(LP_NET_LIBURING_INCLUDE_DIR liburing.h REQUIRED)
    find_library(LP_NET_LIBURING_LIBRARY NAMES uring REQUIRED)
    target_include_directories(LikesProgramNet PRIVATE ${LP_NET_LIBURING_INCLUDE_DIR})
    target_link_libraries(LikesProgramNet PRIVATE ${LP_NET_LIBURING_LIBRARY})

    # 运行时必须加载与配置期一致的 liburing，避免系统并存版本造成 ring ABI 错配。
    get_filename_component(LP_NET_LIBURING_RUNTIME_DIR ${LP_NET_LIBURING_LIBRARY} DIRECTORY)
    set_property(TARGET LikesProgramNet APPEND PROPERTY BUILD_RPATH ${LP_NET_LIBURING_RUNTIME_DIR})
    set_property(TARGET LikesProgramNet APPEND PROPERTY INSTALL_RPATH ${LP_NET_LIBURING_RUNTIME_DIR})
endif()

if (WIN32)
    target_link_libraries(LikesProgramNet PRIVATE Ws2_32 Mswsock)
endif()

# =========================
# 测试程序
# =========================
if (LIKESPROGRAM_BUILD_TESTS)
    # POSIX PackageTests 依赖 socketpair/fork 等接口，Windows 使用独立 IOCP contract。
    if (NOT WIN32)
    add_executable(PackageTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/PackageTests.cpp
    )
    target_include_directories(PackageTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(PackageTests PRIVATE LikesProgram::Net)
    target_compile_definitions(PackageTests PRIVATE LIKESPROGRAM_NET_TEST_HAS_IO_URING=1)
    if (MSVC)
        target_compile_options(PackageTests PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET PackageTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:PackageTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:PackageTests>
        )
    endif()

    add_test(NAME PackageTests COMMAND PackageTests)
    add_test(
        NAME NetDefaultBackendContract
        COMMAND PackageTests --backend-contract
    )
    add_test(
        NAME NetForcedEpollBackendContract
        COMMAND PackageTests --backend-contract epoll
    )
    set_tests_properties(NetForcedEpollBackendContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=epoll"
    )
    add_test(
        NAME NetForcedIoUringBackendContract
        COMMAND PackageTests --backend-contract io_uring
    )
    set_tests_properties(NetForcedIoUringBackendContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=io_uring"
    )
    add_test(
        NAME NetUnknownBackendFallsBackAutoContract
        COMMAND PackageTests --backend-contract auto
    )
    set_tests_properties(NetUnknownBackendFallsBackAutoContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=unknown"
    )
    add_test(
        NAME NetForcedEpollPackageTests
        COMMAND PackageTests
    )
    set_tests_properties(NetForcedEpollPackageTests PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=epoll"
    )
    add_test(
        NAME NetForcedIoUringPackageTests
        COMMAND PackageTests
    )
    set_tests_properties(NetForcedIoUringPackageTests PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=io_uring"
    )
    add_test(
        NAME NetForcedEpollConnectAcceptContract
        COMMAND PackageTests --connect-accept-contract
    )
    set_tests_properties(NetForcedEpollConnectAcceptContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=epoll"
    )
    add_test(
        NAME NetForcedEpollTcpTlsContract
        COMMAND PackageTests --tcp-tls-contract
    )
    set_tests_properties(NetForcedEpollTcpTlsContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=epoll"
    )
    add_test(
        NAME NetForcedEpollUdpDtlsContract
        COMMAND PackageTests --udp-dtls-contract
    )
    set_tests_properties(NetForcedEpollUdpDtlsContract PROPERTIES
        ENVIRONMENT "LIKESPROGRAM_NET_BACKEND=epoll"
    )
    endif()

    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(LinuxPollerBackendPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/LinuxPollerBackendPolicyContractTests.cpp
    )
    target_include_directories(LinuxPollerBackendPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(LinuxPollerBackendPolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME LinuxPollerBackendPolicyContractTests
        COMMAND LinuxPollerBackendPolicyContractTests
    )

    add_executable(SocketOpsContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/SocketOpsContractTests.cpp
    )
    target_include_directories(SocketOpsContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(SocketOpsContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME SocketOpsContractTests
        COMMAND SocketOpsContractTests
    )

    add_executable(ReadinessDriverContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ReadinessDriverContractTests.cpp
    )
    target_include_directories(ReadinessDriverContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(ReadinessDriverContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME ReadinessDriverContractTests
        COMMAND ReadinessDriverContractTests
    )

    add_executable(EpollPollerContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/EpollPollerContractTests.cpp
    )
    target_include_directories(EpollPollerContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(EpollPollerContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME EpollPollerChannelContract
        COMMAND EpollPollerContractTests channel
    )
    add_test(
        NAME EpollPollerTimeoutFireContract
        COMMAND EpollPollerContractTests timeout-fire
    )
    add_test(
        NAME EpollPollerTimeoutCancelContract
        COMMAND EpollPollerContractTests timeout-cancel
    )
    add_test(
        NAME EpollPollerTimeoutEintrContract
        COMMAND EpollPollerContractTests timeout-eintr
    )
    add_test(
        NAME EpollPollerConnectContract
        COMMAND EpollPollerContractTests connect
    )
    add_test(
        NAME EpollPollerConnectCancelContract
        COMMAND EpollPollerContractTests connect-cancel
    )
    add_test(
        NAME EpollPollerConnectFdReuseContract
        COMMAND EpollPollerContractTests connect-fd-reuse
    )
    add_test(
        NAME EpollPollerAcceptContract
        COMMAND EpollPollerContractTests accept
    )
    add_test(
        NAME EpollPollerAcceptStopContract
        COMMAND EpollPollerContractTests accept-stop
    )
    add_test(
        NAME EpollPollerEventLoopWakeupContract
        COMMAND EpollPollerContractTests event-loop-wakeup
    )

    add_executable(ProvidedBufferGenerationContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ProvidedBufferGenerationContractTests.cpp
    )
    target_include_directories(ProvidedBufferGenerationContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(ProvidedBufferGenerationContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME ProvidedBufferGenerationContractTests
        COMMAND ProvidedBufferGenerationContractTests
    )

    add_executable(WriteQueuePolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/WriteQueuePolicyContractTests.cpp
    )
    target_include_directories(WriteQueuePolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(WriteQueuePolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME WriteQueuePolicyContractTests
        COMMAND WriteQueuePolicyContractTests
    )

    add_executable(PollerAccessContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/PollerAccessContractTests.cpp
    )
    target_include_directories(PollerAccessContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(PollerAccessContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME PollerAccessContractTests
        COMMAND PollerAccessContractTests
    )

    add_executable(OperationCancellationPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/OperationCancellationPolicyContractTests.cpp
    )
    target_include_directories(OperationCancellationPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(OperationCancellationPolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME OperationCancellationPolicyContractTests
        COMMAND OperationCancellationPolicyContractTests
    )

    add_executable(ConnectionIdentityPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ConnectionIdentityPolicyContractTests.cpp
    )
    target_include_directories(ConnectionIdentityPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(ConnectionIdentityPolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME ConnectionIdentityPolicyContractTests
        COMMAND ConnectionIdentityPolicyContractTests
    )

    add_executable(DatagramCompletionPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DatagramCompletionPolicyContractTests.cpp
    )
    target_include_directories(DatagramCompletionPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(DatagramCompletionPolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME DatagramCompletionPolicyContractTests
        COMMAND DatagramCompletionPolicyContractTests
    )

    add_executable(DatagramBatchPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DatagramBatchPolicyContractTests.cpp
    )
    target_include_directories(DatagramBatchPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(DatagramBatchPolicyContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME DatagramBatchPolicyContractTests
        COMMAND DatagramBatchPolicyContractTests
    )

    add_executable(DtlsDatagramBatchContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DtlsDatagramBatchContractTests.cpp
    )
    target_link_libraries(DtlsDatagramBatchContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME DtlsDatagramBatchContractTests
        COMMAND DtlsDatagramBatchContractTests
    )

    add_executable(DtlsResultContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DtlsResultContractTests.cpp
    )
    target_link_libraries(DtlsResultContractTests PRIVATE LikesProgram::Net)
    add_test(
        NAME DtlsResultContractTests
        COMMAND DtlsResultContractTests
    )

    add_executable(DtlsSessionPolicyContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/DtlsSessionPolicyContractTests.cpp
    )
    target_link_libraries(DtlsSessionPolicyContractTests PRIVATE LikesProgram::Net)
    target_include_directories(DtlsSessionPolicyContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    add_test(
        NAME DtlsSessionPolicyContractTests
        COMMAND DtlsSessionPolicyContractTests
    )
    endif()

    add_executable(ProxyRelayContractTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/ProxyRelayContractTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ProxyRelayBenchmark.cpp
    )
    target_include_directories(ProxyRelayContractTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(ProxyRelayContractTests PRIVATE LikesProgram::Net)
    if (MSVC)
        target_compile_options(ProxyRelayContractTests PRIVATE /utf-8 /W4)
    endif()
    add_test(NAME ProxyRelayContractTests COMMAND ProxyRelayContractTests)

    if (WIN32)
        add_executable(WindowsPollerBackendPolicyContractTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/WindowsPollerBackendPolicyContractTests.cpp
        )
        target_include_directories(WindowsPollerBackendPolicyContractTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        )
        target_sources(WindowsPollerBackendPolicyContractTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/WindowsPollerBackendPolicy.cpp
        )
        target_link_libraries(WindowsPollerBackendPolicyContractTests PRIVATE LikesProgram::Net)
        add_test(
            NAME WindowsPollerBackendPolicyContractTests
            COMMAND WindowsPollerBackendPolicyContractTests
        )

        add_executable(IocpPollerContractTests
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/IocpPollerContractTests.cpp
        )
        target_include_directories(IocpPollerContractTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        )
        target_sources(IocpPollerContractTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/IocpPoller.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/windows/SocketOps.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src/PollerAccess.cpp
        )
        target_link_libraries(IocpPollerContractTests PRIVATE LikesProgram::Net)
        target_link_libraries(IocpPollerContractTests PRIVATE Ws2_32 Mswsock)
        target_compile_options(IocpPollerContractTests PRIVATE /utf-8 /W4)
        add_test(
            NAME IocpPollerContractTests
            COMMAND IocpPollerContractTests
        )
    endif()
endif()

# =========================
# 示例程序
# =========================
if (LIKESPROGRAM_BUILD_EXAMPLES)
    add_executable(Example
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/Example.cpp
    )
    target_link_libraries(Example PRIVATE LikesProgram::Net)
    if (MSVC)
        target_compile_options(Example PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET Example POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:Example>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:Example>
        )
    endif()

    # Windows shared 构建：benchmark 需要包内 UDP batch 私有实现，使用静态支持库避免扩大 DLL ABI。
    set(LP_NET_BENCHMARK_TARGET LikesProgram::Net)
    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_library(LikesProgramNetBenchmarkSupport STATIC ${LP_NET_SOURCES})
        target_include_directories(LikesProgramNetBenchmarkSupport
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/include
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/include
        )
        target_link_libraries(LikesProgramNetBenchmarkSupport
            PUBLIC LikesProgram::Core
            PRIVATE Ws2_32 Mswsock
        )
        target_compile_options(LikesProgramNetBenchmarkSupport PRIVATE /utf-8 /W4)
        set(LP_NET_BENCHMARK_TARGET LikesProgramNetBenchmarkSupport)
    endif()

    add_executable(Benchmark
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/Benchmark.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/ProxyRelayBenchmark.cpp
    )
    target_include_directories(Benchmark PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
    )
    target_link_libraries(Benchmark PRIVATE ${LP_NET_BENCHMARK_TARGET})
    if (MSVC)
        target_compile_options(Benchmark PRIVATE /utf-8 /W4)
    endif()

    if (LIKESPROGRAM_BUILD_TESTS)
        add_test(
            NAME LikesProgramNetBenchmarkBackendSmoke
            COMMAND Benchmark
        )
        set_tests_properties(LikesProgramNetBenchmarkBackendSmoke PROPERTIES
            PASS_REGULAR_EXPRESSION
                "backend=(iocp-overlapped|epoll-level-completion|io_uring-multishot-provided-buffer);tls_engine_contract .* state=closed unit=contract;dtls_engine_contract .* state=closed unit=contract"
        )
    endif()

    add_executable(HttpBenchmarkServer
        ${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/HttpBenchmarkServer.cpp
    )
    target_link_libraries(HttpBenchmarkServer PRIVATE LikesProgram::Net)
    if (MSVC)
        target_compile_options(HttpBenchmarkServer PRIVATE /utf-8 /W4)
    endif()

    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET Benchmark POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:Benchmark>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:Benchmark>
        )

        add_custom_command(TARGET HttpBenchmarkServer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:HttpBenchmarkServer>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:HttpBenchmarkServer>
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
