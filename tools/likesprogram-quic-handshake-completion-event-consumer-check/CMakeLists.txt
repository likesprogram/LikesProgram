cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicHandshakeCompletionEventConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicHandshakeCompletionEventConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicHandshakeCompletionEventConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicHandshakeCompletionEventConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-handshake-completion-event-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicHandshakeCompletionEventConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicHandshakeCompletionEventConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
