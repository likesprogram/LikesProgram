cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicRetryVersionRoutingConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicRetryVersionRoutingConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicRetryVersionRoutingConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicRetryVersionRoutingConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-retry-version-routing-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicRetryVersionRoutingConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicRetryVersionRoutingConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
