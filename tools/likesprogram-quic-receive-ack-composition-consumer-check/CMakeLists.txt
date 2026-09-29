cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicReceiveAckCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicReceiveAckCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicReceiveAckCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicReceiveAckCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-receive-ack-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicReceiveAckCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicReceiveAckCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
