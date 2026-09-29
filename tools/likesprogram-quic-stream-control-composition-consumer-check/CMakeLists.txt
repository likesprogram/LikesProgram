cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicStreamControlCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicStreamControlCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicStreamControlCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicStreamControlCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-stream-control-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicStreamControlCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicStreamControlCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
