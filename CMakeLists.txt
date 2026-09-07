cmake_minimum_required(VERSION 3.16)
project(tuntom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tuntom src/main.cpp)
target_compile_options(tuntom PRIVATE -Wall -Wextra -pedantic)

add_executable(tuntom-switch src/switch/main.cpp)
target_compile_options(tuntom-switch PRIVATE -Wall -Wextra -pedantic)

add_executable(tuntom-switch-adapter src/adapter/main.cpp)
target_compile_options(tuntom-switch-adapter PRIVATE -Wall -Wextra -pedantic)

add_executable(tuntomctl src/control/main.cpp)
target_compile_options(tuntomctl PRIVATE -Wall -Wextra -pedantic)

include(CTest)
if(BUILD_TESTING)
    foreach(name compact_protocol_test switch_protocol_test switch_options_test exit_adapter_test stats_control_test session_stats_test x25519_test akdf_test pfs_session_test replay_test mac_test aead_test encrypted_session_test session_test processing_stats_test throughput_stats_test adaptive_polling_test)
        add_executable(${name} tests/${name}.cpp)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -pedantic)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
