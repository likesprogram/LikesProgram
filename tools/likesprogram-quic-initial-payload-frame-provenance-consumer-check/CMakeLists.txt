cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-initial-payload-frame-provenance-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicInitialPayloadFrameProvenanceConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
