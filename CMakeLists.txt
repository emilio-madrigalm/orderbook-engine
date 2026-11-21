cmake_minimum_required(VERSION 3.20)
project(orderbook-engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release so benchmarks are never accidentally run unoptimized.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Core library
add_library(orderbook src/order_book.cpp)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(orderbook PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(orderbook PRIVATE -Wall -Wextra -Wpedantic)
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
