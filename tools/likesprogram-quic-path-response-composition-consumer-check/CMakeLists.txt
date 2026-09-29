cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicPathResponseCompositionConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicPathResponseCompositionConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicPathResponseCompositionConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicPathResponseCompositionConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-path-response-composition-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicPathResponseCompositionConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicPathResponseCompositionConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
