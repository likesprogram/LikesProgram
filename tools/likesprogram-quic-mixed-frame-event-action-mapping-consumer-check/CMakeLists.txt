cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-mixed-frame-event-action-mapping-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicMixedFrameEventActionMappingConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
