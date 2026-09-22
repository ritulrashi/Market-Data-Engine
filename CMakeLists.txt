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

add_executable(market_data_server src/main_server.cpp)
target_link_libraries(market_data_server PRIVATE mde_core)

add_executable(market_data_client src/main_client.cpp)
target_link_libraries(market_data_client PRIVATE mde_core)

enable_testing()
add_executable(unit_tests
    tests/test_main.cpp
    tests/test_ring_buffer.cpp
    tests/test_protocol.cpp
)
target_link_libraries(unit_tests PRIVATE mde_core)
add_test(NAME unit_tests COMMAND unit_tests)
