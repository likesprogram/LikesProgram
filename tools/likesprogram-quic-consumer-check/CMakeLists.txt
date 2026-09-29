cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicConsumerCheck PRIVATE LikesProgram::Quic)
set_target_properties(LikesProgramQuicConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicConsumerCheck PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
