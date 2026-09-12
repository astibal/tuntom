cmake_minimum_required(VERSION 3.16)
project(tuntom LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
link_libraries(Threads::Threads)

add_executable(tuntom src/main.cpp)
target_compile_options(tuntom PRIVATE -Wall -Wextra -pedantic)

add_executable(tuntom-switch src/switch/main.cpp)
target_compile_options(tuntom-switch PRIVATE -Wall -Wextra -pedantic)

add_executable(tomtom-switch-mp src/switch_mp/main.cpp)
target_compile_options(tomtom-switch-mp PRIVATE -Wall -Wextra -Wconversion -pedantic)

add_executable(tuntom-switch-adapter src/adapter/main.cpp)
target_compile_options(tuntom-switch-adapter PRIVATE -Wall -Wextra -pedantic)

add_executable(tuntomctl src/control/main.cpp)
target_compile_options(tuntomctl PRIVATE -Wall -Wextra -pedantic)

include(CTest)
if(BUILD_TESTING)
    add_executable(switch_mp_plan_fixture tools/switch_mp_plan.cpp)
    target_compile_options(switch_mp_plan_fixture PRIVATE -Wall -Wextra -Wconversion -pedantic)
    add_executable(switch_mp_plan_test tests/switch_mp_plan_test.cpp)
    target_compile_options(switch_mp_plan_test PRIVATE -Wall -Wextra -Wconversion -pedantic)
    add_test(NAME switch_mp_plan_test COMMAND switch_mp_plan_test)
    add_executable(switch_mp_test tests/switch_mp_test.cpp)
    target_compile_options(switch_mp_test PRIVATE -Wall -Wextra -Wconversion -pedantic)
    add_test(NAME switch_mp_test COMMAND switch_mp_test)
    set_tests_properties(switch_mp_test PROPERTIES TIMEOUT 30)
    foreach(name udp_batch_test compact_protocol_test switch_protocol_test switch_options_test switch_client_test switch_admission_test runtime_state_test logging_test exit_adapter_test stats_control_test session_stats_test session_latency_test x25519_test akdf_test pfs_session_test replay_test mac_test aead_test encrypted_session_test session_test processing_stats_test throughput_stats_test adaptive_polling_test reassembly_test)
        add_executable(${name} tests/${name}.cpp)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wconversion -pedantic)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
    target_link_options(switch_client_test PRIVATE
        -Wl,--wrap=connect -Wl,--wrap=send -Wl,--wrap=sendmsg -Wl,--wrap=getsockopt)
    target_link_options(udp_batch_test PRIVATE -Wl,--wrap=sendmmsg -Wl,--wrap=sendto)
    target_link_options(runtime_state_test PRIVATE -Wl,--wrap=getrandom)
    target_link_options(logging_test PRIVATE -Wl,--wrap=write -Wl,--wrap=pthread_create)
    set_tests_properties(logging_test PROPERTIES TIMEOUT 40)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME mk_switch_mp_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mk_switch_mp_test.py $<TARGET_FILE:switch_mp_plan_fixture>)
        set_tests_properties(mk_switch_mp_test PROPERTIES TIMEOUT 30)
        add_test(NAME mk_switch_replacement_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/mk_switch_replacement_test.py
            $<TARGET_FILE:tuntom-switch> $<TARGET_FILE:tomtom-switch-mp>
            $<TARGET_FILE:tuntomctl> $<TARGET_FILE:switch_mp_plan_fixture>)
        set_tests_properties(mk_switch_replacement_test PROPERTIES TIMEOUT 300)
        add_test(NAME switch_mp_integration_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_mp_integration_test.py
            $<TARGET_FILE:tomtom-switch-mp>)
        set_tests_properties(switch_mp_integration_test PROPERTIES TIMEOUT 90)
        add_test(NAME switch_mp_tunnel_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_tunnel_test.py
            $<TARGET_FILE:tuntom> $<TARGET_FILE:tomtom-switch-mp> $<TARGET_FILE:tuntomctl>)
        set_tests_properties(switch_mp_tunnel_test PROPERTIES TIMEOUT 30)
        add_test(NAME switch_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_test.py
            $<TARGET_FILE:tuntom-switch> $<TARGET_FILE:tuntomctl>)
        set_tests_properties(switch_test PROPERTIES TIMEOUT 15)
        add_test(NAME switch_tunnel_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_tunnel_test.py
            $<TARGET_FILE:tuntom> $<TARGET_FILE:tuntom-switch> $<TARGET_FILE:tuntomctl>)
        set_tests_properties(switch_tunnel_test PROPERTIES TIMEOUT 30)
        add_library(accept_faults SHARED tests/accept_faults.cpp)
        target_link_libraries(accept_faults PRIVATE ${CMAKE_DL_LIBS})
        add_test(NAME switch_capacity_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_capacity_test.py
            $<TARGET_FILE:tuntom-switch> $<TARGET_FILE:accept_faults>)
        set_tests_properties(switch_capacity_test PROPERTIES TIMEOUT 90)
        add_library(runtime_faults SHARED tests/runtime_faults.cpp)
        target_link_libraries(runtime_faults PRIVATE ${CMAKE_DL_LIBS})
        add_library(logging_faults SHARED tests/logging_faults.cpp)
        target_link_libraries(logging_faults PRIVATE ${CMAKE_DL_LIBS})
        add_executable(adapter_reconnect_fixture src/adapter/main.cpp tests/adapter_tun_fixture.cpp)
        target_compile_options(adapter_reconnect_fixture PRIVATE -Wall -Wextra -Wconversion -pedantic)
        target_link_options(adapter_reconnect_fixture PRIVATE -Wl,--wrap=open -Wl,--wrap=ioctl)
        add_test(NAME switch_reconnect_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/switch_reconnect_test.py
            $<TARGET_FILE:tuntom> $<TARGET_FILE:adapter_reconnect_fixture> $<TARGET_FILE:tuntom-switch>)
        set_tests_properties(switch_reconnect_test PROPERTIES TIMEOUT 40)
        add_test(NAME runtime_recovery_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_recovery_test.py
            $<TARGET_FILE:tuntom> $<TARGET_FILE:tuntom-switch>
            $<TARGET_FILE:adapter_reconnect_fixture> $<TARGET_FILE:runtime_faults>)
        set_tests_properties(runtime_recovery_test PROPERTIES TIMEOUT 90)
        add_test(NAME logging_recovery_test COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/logging_recovery_test.py
            $<TARGET_FILE:tuntom> $<TARGET_FILE:tuntom-switch>
            $<TARGET_FILE:adapter_reconnect_fixture> $<TARGET_FILE:logging_faults>)
        set_tests_properties(logging_recovery_test PROPERTIES TIMEOUT 60)
    endif()
endif()
