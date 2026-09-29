cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-mapped-event-action-delivery-replay-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicMappedEventActionDeliveryReplayConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
