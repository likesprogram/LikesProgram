cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicFlowControlCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicFlowControlCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicFlowControlCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicFlowControlCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-flow-control-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicFlowControlCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicFlowControlCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
