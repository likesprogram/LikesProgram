cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-initial-header-packet-number-payload-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicInitialHeaderPacketNumberPayloadConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
