cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-padding-frame-sequence-dispatch-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicPaddingFrameSequenceDispatchConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
