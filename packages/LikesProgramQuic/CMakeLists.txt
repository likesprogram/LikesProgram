# ======================================================
#
# LikesProgramQuic
#
# ======================================================

file(GLOB_RECURSE LP_QUIC_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Quic/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/LikesProgram/Quic/*.h
)

file(GLOB_RECURSE LP_QUIC_PRIVATE_HEADERS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/*.h
)

file(GLOB_RECURSE LP_QUIC_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
)

if (NOT TARGET LikesProgram::Net)
    message(FATAL_ERROR "LikesProgramQuic requires LikesProgramNet")
endif()

if (LIKESPROGRAM_BUILD_SHARED)
    add_library(LikesProgramQuic SHARED ${LP_QUIC_SOURCES})
    target_compile_definitions(LikesProgramQuic
        PUBLIC LIKESPROGRAM_QUIC_SHARED
        PRIVATE LIKESPROGRAM_QUIC_EXPORTS
    )
else()
    add_library(LikesProgramQuic STATIC ${LP_QUIC_SOURCES})
endif()

add_library(LikesProgram::Quic ALIAS LikesProgramQuic)

target_sources(LikesProgramQuic PRIVATE ${LP_QUIC_HEADERS} ${LP_QUIC_PRIVATE_HEADERS})
target_link_libraries(LikesProgramQuic PUBLIC LikesProgram::Net)
target_include_directories(LikesProgramQuic
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/include
)

set_target_properties(LikesProgramQuic PROPERTIES
    EXPORT_NAME Quic
    OUTPUT_NAME "LikesProgramQuic"
    DEBUG_POSTFIX "d"
)

if (NOT LIKESPROGRAM_BUILD_SHARED)
    set_target_properties(LikesProgramQuic PROPERTIES
        OUTPUT_NAME "LikesProgramQuicStatic"
    )
endif()

if (MSVC)
    target_compile_options(LikesProgramQuic PRIVATE /utf-8 /W4)
else()
    target_compile_options(LikesProgramQuic PRIVATE -Wall -Wextra -Wpedantic)
endif()

if (LIKESPROGRAM_BUILD_TESTS)
    add_executable(LikesProgramQuicTests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicEngineContractTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicWireEngineTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicLongHeaderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPacketNumberTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPacketNumberSpaceTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicShortHeaderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPacketHeaderProtectionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicRetransmissionQueueTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicCongestionBudgetTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicFrameTypeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicAckFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicAckApplicationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicAckDelayTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicStreamFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicCryptoFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicStreamControlFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicConnectionCloseFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicConnectionIdFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicDatagramFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicFlowControlFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicHandshakeDoneFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicTimeoutTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicStreamCreditTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicConnectionCreditTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicCreditReservationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicAckTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicAckRecoveryLedgerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicCreditCongestionLedgerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPathMigrationLedgerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicConnectionIdTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPathProbeTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicStreamTerminalTrackerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPathValidationFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicPingFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicNewTokenFrameTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/QuicVarIntTests.cpp
    )
    target_link_libraries(LikesProgramQuicTests PRIVATE LikesProgram::Quic)
    if (MSVC)
        target_compile_options(LikesProgramQuicTests PRIVATE /utf-8 /W4)
    endif()
    if (WIN32 AND LIKESPROGRAM_BUILD_SHARED)
        add_custom_command(TARGET LikesProgramQuicTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramCore>
                $<TARGET_FILE_DIR:LikesProgramQuicTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramNet>
                $<TARGET_FILE_DIR:LikesProgramQuicTests>
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LikesProgramQuic>
                $<TARGET_FILE_DIR:LikesProgramQuicTests>
        )
    endif()
    add_test(NAME LikesProgramQuicTests COMMAND LikesProgramQuicTests)
endif()

install(TARGETS LikesProgramQuic
    EXPORT LikesProgramTargets
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/LikesProgram DESTINATION include)
