cmake_minimum_required(VERSION 3.16)
project(miniLB VERSION 0.1.0 LANGUAGES CXX)

# ---- Compiler settings ------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# ---- Sanitizers ---------------------------------------------------------------
# cmake -DMINILB_SANITIZER=thread   -> ThreadSanitizer (data races, deadlocks)
# cmake -DMINILB_SANITIZER=address  -> AddressSanitizer + UBSan (memory errors,
#                                      leaks, undefined behaviour)
# The flags are set before any target is defined so that the bundled
# redis-plus-plus is instrumented as well.
set(MINILB_SANITIZER "" CACHE STRING "Build with a sanitizer: thread | address")
if(MINILB_SANITIZER STREQUAL "thread")
  # -Wno-tsan: GCC warns that Boost.Asio's use of atomic_thread_fence cannot
  # be modelled by TSan. That is inside Asio, not our code, and only noise.
  add_compile_options(-fsanitize=thread -g -fno-omit-frame-pointer -Wno-tsan)
  add_link_options(-fsanitize=thread)
elseif(MINILB_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -g
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(MINILB_SANITIZER)
  message(FATAL_ERROR "MINILB_SANITIZER must be 'thread' or 'address'")
endif()

# ---- Dependencies -----------------------------------------------------------
# Boost.Asio is header-only, so we only need the Boost headers.
find_package(Boost REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
# yaml-cpp 0.8+ exports "yaml-cpp::yaml-cpp"; older releases export "yaml-cpp".
if(TARGET yaml-cpp::yaml-cpp)
  set(MINILB_YAML_TARGET yaml-cpp::yaml-cpp)
else()
  set(MINILB_YAML_TARGET yaml-cpp)
endif()

# redis-plus-plus (C++ Redis client on top of hiredis). Many distributions do
# not package it, so fall back to downloading and building it from source.
find_package(redis++ QUIET)
if(redis++_FOUND)
  message(STATUS "Using system redis-plus-plus")
  set(MINILB_REDIS_TARGET redis++::redis++)
else()
  message(STATUS "redis-plus-plus not found, fetching it from GitHub")
  include(FetchContent)
  set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
  set(REDIS_PLUS_PLUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(REDIS_PLUS_PLUS_CXX_STANDARD 17 CACHE STRING "" FORCE)
  FetchContent_Declare(
    redis_plus_plus
    GIT_REPOSITORY https://github.com/sewenew/redis-plus-plus.git
    GIT_TAG 1.3.15
    GIT_SHALLOW TRUE
  )
  FetchContent_MakeAvailable(redis_plus_plus)
  set(MINILB_REDIS_TARGET redis++::redis++_static)
endif()
# redis-plus-plus is a wrapper around the C library hiredis, which the final
# binary must link as well.
find_library(HIREDIS_LIBRARY hiredis REQUIRED)

# ---- Library: everything except main() --------------------------------------
# Keeping the code in a library lets the tests link against the same objects
# as the real binary.
add_library(minilb_lib STATIC
  src/balancer/Backend.cpp
  src/balancer/BackendPool.cpp
  src/balancer/LeastConnections.cpp
  src/balancer/RoundRobin.cpp
  src/balancer/Strategy.cpp
  src/balancer/WeightedRoundRobin.cpp
  src/core/Connection.cpp
  src/core/EventLoop.cpp
  src/core/Server.cpp
  src/core/ThreadPool.cpp
  src/health/HealthChecker.cpp
  src/http/Headers.cpp
  src/http/HttpParser.cpp
  src/http/HttpRequest.cpp
  src/http/HttpResponse.cpp
  src/redis/Cache.cpp
  src/redis/RateLimiter.cpp
  src/redis/RedisClient.cpp
  src/util/Config.cpp
  src/util/Logger.cpp
  src/util/Metrics.cpp
)
target_include_directories(minilb_lib PUBLIC include)
target_link_libraries(minilb_lib
  PUBLIC Boost::headers Threads::Threads spdlog::spdlog
  PRIVATE ${MINILB_REDIS_TARGET} ${HIREDIS_LIBRARY} ${MINILB_YAML_TARGET}
)

# ---- The load balancer binary -----------------------------------------------
add_executable(miniLB src/main.cpp)
target_link_libraries(miniLB PRIVATE minilb_lib)

# ---- Demo backend: a tiny HTTP server to load-balance across ----------------
add_executable(demo_backend backends/demo_backend.cpp)
target_link_libraries(demo_backend PRIVATE minilb_lib)

# ---- Tests ---------------------------------------------------------------------
option(MINILB_BUILD_TESTS "Build the unit and integration tests" ON)
if(MINILB_BUILD_TESTS)
  enable_testing()

  # Use the system GoogleTest if there is one, otherwise download it.
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found, fetching it from GitHub")
    include(FetchContent)
    FetchContent_Declare(
      googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
      GIT_SHALLOW TRUE
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  # Unit tests: no network, no Redis needed.
  add_executable(unit_tests
    tests/unit/CacheTest.cpp
    tests/unit/ConfigTest.cpp
    tests/unit/HttpParserTest.cpp
    tests/unit/RateLimiterTest.cpp
    tests/unit/StrategyTest.cpp
    tests/unit/ThreadPoolTest.cpp
  )
  target_link_libraries(unit_tests PRIVATE minilb_lib GTest::gtest_main)
  target_compile_definitions(unit_tests PRIVATE MINILB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME unit_tests COMMAND unit_tests)
  set_tests_properties(unit_tests PROPERTIES TIMEOUT 300)

  # Integration tests: start the real binaries (and redis-server if installed).
  add_executable(integration_tests tests/integration/IntegrationTest.cpp)
  target_link_libraries(integration_tests PRIVATE minilb_lib GTest::gtest_main)
  target_compile_definitions(integration_tests PRIVATE
    MINILB_BIN="$<TARGET_FILE:miniLB>"
    DEMO_BACKEND_BIN="$<TARGET_FILE:demo_backend>"
  )
  add_dependencies(integration_tests miniLB demo_backend)
  add_test(NAME integration_tests COMMAND integration_tests)
  set_tests_properties(integration_tests PROPERTIES TIMEOUT 900)
endif()
