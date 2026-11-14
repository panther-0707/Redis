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

# ---- Coverage -----------------------------------------------------------------
# cmake -DMINILB_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug  -> gcov instrumentation.
# Set here, after the dependencies, so that only miniLB's own code is
# instrumented (not the bundled redis-plus-plus).
# See software-testing/scripts/coverage.sh.
option(MINILB_COVERAGE "Instrument miniLB for gcov/gcovr coverage reports" OFF)
if(MINILB_COVERAGE)
  # -fprofile-update=atomic: miniLB is multi-threaded. With the default
  # (non-atomic) counters two threads can lose or corrupt an update, which
  # shows up as wrong or even negative execution counts.
  add_compile_options(--coverage -fprofile-update=atomic -O0 -g)
  add_link_options(--coverage)
endif()

# ---- Assertions -----------------------------------------------------------------
# MINILB_ASSERT (include/miniLB/util/Assert.hpp) checks internal invariants and
# aborts when one is broken. On in every build except Release, so the
# sanitizer, coverage and debug builds all run with the checks.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(MINILB_ASSERTIONS_DEFAULT OFF)
else()
  set(MINILB_ASSERTIONS_DEFAULT ON)
endif()
option(MINILB_ASSERTIONS "Check internal invariants at run time" ${MINILB_ASSERTIONS_DEFAULT})
if(MINILB_ASSERTIONS)
  add_compile_definitions(MINILB_ASSERTIONS)
endif()

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
    # Added for the Software Testing coursework (see software-testing/README.md)
    software-testing/tests/unit/AssertionTest.cpp
    software-testing/tests/unit/BoundaryValueTest.cpp
    software-testing/tests/unit/CoverageGapUnitTest.cpp
    software-testing/tests/unit/FuzzFindingsTest.cpp
    software-testing/tests/unit/HealthStateMachineTest.cpp
    software-testing/tests/unit/MutationKillTest.cpp
  )
  target_include_directories(unit_tests PRIVATE tests/unit)
  target_link_libraries(unit_tests PRIVATE minilb_lib GTest::gtest_main)

  # Property-based tests need RapidCheck (apt-get install librapidcheck-dev).
  # They are an extra: without the library everything else still builds.
  find_package(rapidcheck QUIET)
  if(TARGET rapidcheck AND TARGET rapidcheck_gtest)
    message(STATUS "RapidCheck found: building the property-based tests")
    target_sources(unit_tests PRIVATE software-testing/tests/unit/PropertyTest.cpp)
    target_link_libraries(unit_tests PRIVATE rapidcheck rapidcheck_gtest)
    set(MINILB_HAVE_RAPIDCHECK ON)
  else()
    message(STATUS "RapidCheck not found: property-based tests are not built")
  endif()
  target_compile_definitions(unit_tests PRIVATE MINILB_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME unit_tests COMMAND unit_tests)
  set_tests_properties(unit_tests PROPERTIES TIMEOUT 300)
  if(MINILB_HAVE_RAPIDCHECK)
    # 2000 generated cases per property instead of RapidCheck's default 100
    # (a few seconds in total). Defect D1 needed about 2300 cases to show up.
    set_tests_properties(unit_tests PROPERTIES ENVIRONMENT "RC_PARAMS=max_success=2000")
  endif()

  # Integration tests: start the real binaries (and redis-server if installed).
  add_executable(integration_tests
    tests/integration/IntegrationTest.cpp
    # Added for the Software Testing coursework (see software-testing/README.md)
    software-testing/tests/integration/CoverageGapTest.cpp
    software-testing/tests/integration/PairwiseConfigTest.cpp
    software-testing/tests/integration/RetryAndForwardingTest.cpp
    software-testing/tests/integration/ReviewFindingsTest.cpp
  )
  target_include_directories(integration_tests PRIVATE tests/integration)
  target_link_libraries(integration_tests PRIVATE minilb_lib GTest::gtest_main)
  target_compile_definitions(integration_tests PRIVATE
    MINILB_BIN="$<TARGET_FILE:miniLB>"
    DEMO_BACKEND_BIN="$<TARGET_FILE:demo_backend>"
  )
  add_dependencies(integration_tests miniLB demo_backend)
  add_test(NAME integration_tests COMMAND integration_tests)
  set_tests_properties(integration_tests PROPERTIES TIMEOUT 900)
endif()
