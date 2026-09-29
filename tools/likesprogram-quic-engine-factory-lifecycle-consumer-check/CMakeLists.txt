cmake_minimum_required(VERSION 3.15)

project(LikesProgramQuicEngineFactoryLifecycleConsumerCheck LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LikesProgram CONFIG REQUIRED)

add_executable(LikesProgramQuicEngineFactoryLifecycleConsumerCheck main.cpp)
target_link_libraries(LikesProgramQuicEngineFactoryLifecycleConsumerCheck PRIVATE
    LikesProgram::Quic)
set_target_properties(LikesProgramQuicEngineFactoryLifecycleConsumerCheck PROPERTIES
    OUTPUT_NAME "likesprogram-quic-engine-factory-lifecycle-consumer-check")

if (MSVC)
    target_compile_options(LikesProgramQuicEngineFactoryLifecycleConsumerCheck PRIVATE
        /utf-8 /W4 /WX)
else()
    target_compile_options(LikesProgramQuicEngineFactoryLifecycleConsumerCheck PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
endif()
