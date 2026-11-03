cmake_minimum_required(VERSION 3.16)
project(MultiNet VERSION 0.4.0 LANGUAGES C)

include(CTest)
include(CMakePackageConfigHelpers)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(multinet STATIC src/multinet.c)
add_library(MultiNet::MultiNet ALIAS multinet)
set_target_properties(multinet PROPERTIES EXPORT_NAME MultiNet)
target_include_directories(multinet PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
if(NOT WIN32)
    target_compile_definitions(multinet PRIVATE _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
endif()

if(WIN32)
    target_compile_definitions(multinet PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(multinet PUBLIC ws2_32 bcrypt iphlpapi)
elseif(UNIX)
    target_link_libraries(multinet PUBLIC m)
endif()

if(MSVC)
    target_compile_options(multinet PRIVATE /W4)
else()
    target_compile_options(multinet PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

option(MULTINET_BUILD_DEMO "Build the interactive LAN multiplayer demo" ON)
option(MULTINET_BUILD_TESTS "Build MultiNet tests" ON)

if(MULTINET_BUILD_DEMO)
    add_executable(multinet_demo examples/multinet_demo.c)
    target_link_libraries(multinet_demo PRIVATE MultiNet::MultiNet)
    install(TARGETS multinet_demo RUNTIME DESTINATION bin)
    if(NOT WIN32)
        target_compile_definitions(multinet_demo PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(MSVC)
        target_compile_options(multinet_demo PRIVATE /W4)
    else()
        target_compile_options(multinet_demo PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(multinet_chat examples/multinet_chat.c)
    target_link_libraries(multinet_chat PRIVATE MultiNet::MultiNet)
    install(TARGETS multinet_chat RUNTIME DESTINATION bin)
    add_executable(multinet_2d_object examples/multinet_2d_object.c)
    target_link_libraries(multinet_2d_object PRIVATE MultiNet::MultiNet)
    install(TARGETS multinet_2d_object RUNTIME DESTINATION bin)
    if(NOT WIN32)
        target_compile_definitions(multinet_chat PRIVATE _POSIX_C_SOURCE=200809L)
        target_compile_definitions(multinet_2d_object PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(MSVC)
        target_compile_options(multinet_chat PRIVATE /W4)
        target_compile_options(multinet_2d_object PRIVATE /W4)
    else()
        target_compile_options(multinet_chat PRIVATE -Wall -Wextra -Wpedantic)
        target_compile_options(multinet_2d_object PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTING AND MULTINET_BUILD_TESTS)
    add_executable(multinet_tests tests/test_multinet.c)
    target_link_libraries(multinet_tests PRIVATE MultiNet::MultiNet)
    if(NOT WIN32)
        target_compile_definitions(multinet_tests PRIVATE _POSIX_C_SOURCE=200809L)
    endif()
    if(MSVC)
        target_compile_options(multinet_tests PRIVATE /W4)
    else()
        target_compile_options(multinet_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME multinet_loopback COMMAND multinet_tests)
endif()

option(MULTINET_BUILD_FUZZERS "Build the optional Clang/libFuzzer packet target" OFF)
if(MULTINET_BUILD_FUZZERS)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "MULTINET_BUILD_FUZZERS requires Clang with libFuzzer support")
    endif()
    if(NOT UNIX OR WIN32)
        message(FATAL_ERROR "The current libFuzzer harness is supported on Unix-like systems")
    endif()
    add_executable(multinet_packet_fuzzer tests/fuzz_packets.c)
    target_include_directories(multinet_packet_fuzzer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_definitions(multinet_packet_fuzzer PRIVATE
        _POSIX_C_SOURCE=200809L _DEFAULT_SOURCE=1)
    target_compile_options(multinet_packet_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
        -Wall -Wextra -Wpedantic)
    target_link_options(multinet_packet_fuzzer PRIVATE
        -fsanitize=fuzzer,address,undefined)
    target_link_libraries(multinet_packet_fuzzer PRIVATE m)
endif()

install(TARGETS multinet EXPORT MultiNetTargets ARCHIVE DESTINATION lib)
install(FILES include/multinet.h DESTINATION include)
install(EXPORT MultiNetTargets NAMESPACE MultiNet:: DESTINATION lib/cmake/MultiNet)

configure_package_config_file(
    cmake/MultiNetConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MultiNetConfig.cmake
    INSTALL_DESTINATION lib/cmake/MultiNet)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MultiNetConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MultiNetConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MultiNetConfigVersion.cmake
    DESTINATION lib/cmake/MultiNet)
