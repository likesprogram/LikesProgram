cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-short-header-packet-number-payload-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicShortHeaderPacketNumberPayloadConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
