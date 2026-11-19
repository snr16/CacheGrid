cmake_minimum_required(VERSION 3.20)
project(CacheGrid VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CACHEGRID_BUILD_TESTS "Build unit tests" ON)
set(CACHEGRID_SANITIZER "" CACHE STRING "Sanitizer to enable: address | thread | (empty)")

# ---- Common compile options -------------------------------------------------
add_library(cachegrid_options INTERFACE)
target_compile_options(cachegrid_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

if(CACHEGRID_SANITIZER STREQUAL "address")
  target_compile_options(cachegrid_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(cachegrid_options INTERFACE -fsanitize=address,undefined)
elseif(CACHEGRID_SANITIZER STREQUAL "thread")
  target_compile_options(cachegrid_options INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(cachegrid_options INTERFACE -fsanitize=thread)
elseif(NOT CACHEGRID_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown CACHEGRID_SANITIZER='${CACHEGRID_SANITIZER}' (use address or thread)")
endif()

# ---- Core library -----------------------------------------------------------
find_package(Threads REQUIRED)

add_library(cachegrid_core
  src/Config.cpp
  src/LRUCache.cpp
  src/Cache.cpp
  src/Resp.cpp
  src/CommandHandler.cpp
  src/Net.cpp
  src/Poller.cpp
  src/CacheServer.cpp
  src/HashRing.cpp
  src/ProxyServer.cpp
  src/NodeClient.cpp
  src/LatencyHistogram.cpp
  src/Workload.cpp
  src/LoadGenerator.cpp
  src/ProcessStats.cpp
)
target_include_directories(cachegrid_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cachegrid_core PUBLIC cachegrid_options Threads::Threads)

# ---- Server executable ------------------------------------------------------
add_executable(cachegrid-server src/main.cpp)
target_link_libraries(cachegrid-server PRIVATE cachegrid_core)

add_executable(cachegrid-proxy src/proxy_main.cpp)
target_link_libraries(cachegrid-proxy PRIVATE cachegrid_core)

# ---- Benchmarks -------------------------------------------------------------
add_executable(shard_contention benchmarks/shard_contention.cpp)
target_link_libraries(shard_contention PRIVATE cachegrid_core)
add_executable(cachegrid-bench benchmarks/load_tester.cpp)
target_link_libraries(cachegrid-bench PRIVATE cachegrid_core)
add_executable(ring_analysis benchmarks/ring_analysis.cpp)
target_link_libraries(ring_analysis PRIVATE cachegrid_core)

# ---- Tests ------------------------------------------------------------------
if(CACHEGRID_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  add_executable(cachegrid_tests
    tests/CacheTest.cpp
    tests/ConcurrencyTest.cpp
    tests/ConfigTest.cpp
    tests/RespTest.cpp
    tests/CommandHandlerTest.cpp
    tests/ServerTest.cpp
    tests/HashRingTest.cpp
    tests/ProxyTest.cpp
    tests/NodeClientTest.cpp
    tests/BenchToolsTest.cpp
  )
  target_link_libraries(cachegrid_tests PRIVATE cachegrid_core GTest::gtest_main)
  # Lets tests locate the shipped config files regardless of build directory.
  target_compile_definitions(cachegrid_tests PRIVATE CACHEGRID_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  include(GoogleTest)
  gtest_discover_tests(cachegrid_tests)
endif()
