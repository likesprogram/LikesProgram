cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-padding-mixed-frame-replay-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicPaddingMixedFrameReplayConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
