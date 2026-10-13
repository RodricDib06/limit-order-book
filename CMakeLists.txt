cmake_minimum_required(VERSION 3.16)
project(OrderBook VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ORDERBOOK_BUILD_TESTS "Build the GoogleTest suite" ON)
option(ORDERBOOK_BUILD_BENCH "Build the throughput benchmark" ON)
option(ORDERBOOK_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

if(ORDERBOOK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- Library ---------------------------------------------------------------
add_library(orderbook
  src/OrderBook.cpp
  src/MatchingEngine.cpp
)
target_include_directories(orderbook PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(orderbook PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# ---- Demo app --------------------------------------------------------------
add_executable(orderbook_cli src/main.cpp)
target_link_libraries(orderbook_cli PRIVATE orderbook)
set_target_properties(orderbook_cli PROPERTIES OUTPUT_NAME orderbook)

# ---- Tests -----------------------------------------------------------------
if(ORDERBOOK_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(orderbook_tests
    tests/OrderBookTest.cpp
    tests/MatchingEngineTest.cpp
    tests/StressTest.cpp
  )
  target_link_libraries(orderbook_tests PRIVATE orderbook GTest::gtest_main)
  target_compile_options(orderbook_tests PRIVATE -Wall -Wextra)
  include(GoogleTest)
  gtest_discover_tests(orderbook_tests DISCOVERY_TIMEOUT 60)

  add_test(NAME cli_sample_csv
    COMMAND orderbook_cli ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_orders.csv)
endif()

# ---- Benchmark -------------------------------------------------------------
if(ORDERBOOK_BUILD_BENCH)
  add_executable(orderbook_bench bench/Benchmark.cpp)
  target_link_libraries(orderbook_bench PRIVATE orderbook)
endif()
