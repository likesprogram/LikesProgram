cmake_minimum_required(VERSION 3.15)

project(LikesProgramHttp3ActionQueueConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramHttp3ActionQueueConsumerCheck main.cpp)
target_link_libraries(LikesProgramHttp3ActionQueueConsumerCheck PRIVATE
    LikesProgram::Http)
set_target_properties(LikesProgramHttp3ActionQueueConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-http3-action-queue-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramHttp3ActionQueueConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramHttp3ActionQueueConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
