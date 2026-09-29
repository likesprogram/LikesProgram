cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-crypto-frame-offset-sequence-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicCryptoFrameOffsetSequenceConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
