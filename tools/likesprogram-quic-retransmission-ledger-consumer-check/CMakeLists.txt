cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicRetransmissionLedgerConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicRetransmissionLedgerConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicRetransmissionLedgerConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicRetransmissionLedgerConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-retransmission-ledger-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicRetransmissionLedgerConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicRetransmissionLedgerConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
