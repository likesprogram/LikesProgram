cmake_minimum_required(VERSION 3.15)

project(LikesProgramHttp3QuicConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramHttp3QuicConsumerCheck main.cpp)
target_link_libraries(LikesProgramHttp3QuicConsumerCheck PRIVATE
    LikesProgram::Http
    LikesProgram::Quic)
set_target_properties(LikesProgramHttp3QuicConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-http3-quic-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramHttp3QuicConsumerCheck PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramHttp3QuicConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
