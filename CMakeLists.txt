cmake_minimum_required(VERSION 3.16)
project(concurrent_market_data_distribution_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

option(MDE_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(MDE_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(MDE_ENABLE_COVERAGE "Build with gcov coverage instrumentation" OFF)

find_package(Threads REQUIRED)

add_library(mde_core
    src/producer/tick_producer.cpp
    src/network/server.cpp
    src/client/tick_client.cpp
)
target_include_directories(mde_core PUBLIC include)
target_compile_options(mde_core PUBLIC -Wall -Wextra)
target_link_libraries(mde_core PUBLIC Threads::Threads)

if(MDE_ENABLE_ASAN)
    target_compile_options(mde_core PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(mde_core PUBLIC -fsanitize=address)
endif()

if(MDE_ENABLE_TSAN)
    target_compile_options(mde_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer -g)
    target_link_options(mde_core PUBLIC -fsanitize=thread)
endif()

if(MDE_ENABLE_COVERAGE)
    target_compile_options(mde_core PUBLIC --coverage -fprofile-update=atomic -O0 -g)
    target_link_options(mde_core PUBLIC --coverage)
endif()

add_executable(market_data_server src/main_server.cpp)
target_link_libraries(market_data_server PRIVATE mde_core)

add_executable(market_data_client src/main_client.cpp)
target_link_libraries(market_data_client PRIVATE mde_core)

option(MDE_BUILD_TESTS "Build the GoogleTest suite" ON)
if(MDE_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(unit_tests
        tests/test_ring_buffer.cpp
        tests/test_protocol.cpp
        tests/test_producer.cpp
    )
    target_link_libraries(unit_tests PRIVATE mde_core GTest::gtest_main)

    add_executable(integration_tests
        tests/test_integration.cpp
    )
    target_link_libraries(integration_tests PRIVATE mde_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 60)
    gtest_discover_tests(integration_tests DISCOVERY_TIMEOUT 60 PROPERTIES RUN_SERIAL TRUE)
endif()
