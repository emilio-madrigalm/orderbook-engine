cmake_minimum_required(VERSION 3.20)
project(orderbook-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release so benchmarks are never accidentally run unoptimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optional sanitizers: cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DORDERBOOK_SANITIZE=ON
option(ORDERBOOK_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
if(ORDERBOOK_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Core library
add_library(orderbook src/order_book.cpp)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(orderbook PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(orderbook PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Event schema (CSV) + synthetic generator
add_library(obdata src/event.cpp src/generator.cpp src/replayer.cpp)
target_link_libraries(obdata PUBLIC orderbook)
if(NOT MSVC)
  target_compile_options(obdata PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ob_gen src/gen_main.cpp)
target_link_libraries(ob_gen PRIVATE obdata)
if(NOT MSVC)
  target_compile_options(ob_gen PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ob_replay src/replay_main.cpp)
target_link_libraries(ob_replay PRIVATE obdata)
if(NOT MSVC)
  target_compile_options(ob_replay PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Benchmark (Release only; the binary refuses to run otherwise)
add_executable(ob_bench bench/bench.cpp)
target_link_libraries(ob_bench PRIVATE obdata)
if(NOT MSVC)
  target_compile_options(ob_bench PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Tests (GoogleTest via FetchContent)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()
include(GoogleTest)

add_executable(test_order_book tests/test_order_book.cpp)
target_link_libraries(test_order_book PRIVATE orderbook GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_order_book PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_order_book)

# Property test: OrderBook vs naive reference on random streams
add_executable(test_property tests/test_property.cpp)
target_include_directories(test_property PRIVATE tests)
target_link_libraries(test_property PRIVATE orderbook GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_property PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_property)

# Generator + CSV schema tests
add_executable(test_generator tests/test_generator.cpp)
target_link_libraries(test_generator PRIVATE obdata GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_generator PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_generator)

# Replayer tests
add_executable(test_replayer tests/test_replayer.cpp)
target_link_libraries(test_replayer PRIVATE obdata GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_replayer PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_replayer)

# Node pool allocator tests
add_executable(test_pool_allocator tests/test_pool_allocator.cpp)
target_link_libraries(test_pool_allocator PRIVATE orderbook GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_pool_allocator PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_pool_allocator)

# Tick-band and flat-array tests
add_executable(test_price_band tests/test_price_band.cpp)
target_link_libraries(test_price_band PRIVATE orderbook GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_price_band PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_price_band)

# Id index (flat open-addressing table) stress tests
add_executable(test_id_index tests/test_id_index.cpp)
target_link_libraries(test_id_index PRIVATE orderbook GTest::gtest_main)
if(NOT MSVC)
  target_compile_options(test_id_index PRIVATE -Wall -Wextra -Wpedantic)
endif()
gtest_discover_tests(test_id_index)
