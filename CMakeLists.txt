cmake_minimum_required(VERSION 3.16)
project(tuntom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tuntom src/main.cpp)
target_compile_options(tuntom PRIVATE -Wall -Wextra -pedantic)

include(CTest)
if(BUILD_TESTING)
    foreach(name replay_test mac_test session_test processing_stats_test)
        add_executable(${name} tests/${name}.cpp)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -pedantic)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
