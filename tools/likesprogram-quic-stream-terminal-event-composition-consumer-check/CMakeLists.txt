cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-stream-terminal-event-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicStreamTerminalEventCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
