cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-packet-envelope-sequencing-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicPacketEnvelopeSequencingConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
