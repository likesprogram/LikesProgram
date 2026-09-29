cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicFrameSequenceDispatchConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicFrameSequenceDispatchConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicFrameSequenceDispatchConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicFrameSequenceDispatchConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-frame-sequence-dispatch-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicFrameSequenceDispatchConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicFrameSequenceDispatchConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
