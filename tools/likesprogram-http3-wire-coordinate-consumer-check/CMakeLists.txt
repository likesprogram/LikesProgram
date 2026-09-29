cmake_minimum_required(VERSION 3.15)

project(LikesProgramHttp3WireCoordinateConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramHttp3WireCoordinateConsumerCheck main.cpp)
target_link_libraries(LikesProgramHttp3WireCoordinateConsumerCheck PRIVATE
    LikesProgram::Http
    LikesProgram::Quic)
set_target_properties(LikesProgramHttp3WireCoordinateConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-http3-wire-coordinate-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramHttp3WireCoordinateConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramHttp3WireCoordinateConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
