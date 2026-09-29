cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicConnectionIdCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicConnectionIdCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicConnectionIdCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicConnectionIdCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-connection-id-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicConnectionIdCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicConnectionIdCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
