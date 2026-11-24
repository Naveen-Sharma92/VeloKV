cmake_minimum_required(VERSION 3.16)
project(VeloKV LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(VELOKV_BUILD_TESTS "Build the unit and integration tests" ON)
option(VELOKV_SANITIZE "Build everything with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(VELOKV_WERROR "Treat compiler warnings as errors" OFF)

# Sanitizers are applied globally (including GoogleTest) so that every
# translation unit in a test binary agrees on instrumentation.
if(VELOKV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                      -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Warnings apply only to our own targets, not to third-party code.
set(VELOKV_WARNINGS -Wall -Wextra -Wpedantic)
if(VELOKV_WERROR)
  list(APPEND VELOKV_WARNINGS -Werror)
endif()

# Everything except main() lives in a library so the tests link the same code
# the server runs.
add_library(velokv_core STATIC
  src/aof.cpp
  src/commands.cpp
  src/connection.cpp
  src/expiry.cpp
  src/keyspace.cpp
  src/resp.cpp
  src/server.cpp
  src/skiplist.cpp
)
target_include_directories(velokv_core PUBLIC src)
target_compile_options(velokv_core PRIVATE ${VELOKV_WARNINGS})

add_executable(velokv src/main.cpp)
target_link_libraries(velokv PRIVATE velokv_core)
target_compile_options(velokv PRIVATE ${VELOKV_WARNINGS})

if(VELOKV_BUILD_TESTS)
  enable_testing()

  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  find_package(Threads REQUIRED)

  add_executable(velokv_tests
    tests/test_aof.cpp
    tests/test_commands.cpp
    tests/test_expiry.cpp
    tests/test_fd.cpp
    tests/test_keyspace.cpp
    tests/test_resp.cpp
    tests/test_server.cpp
    tests/test_skiplist.cpp
  )
  target_link_libraries(velokv_tests PRIVATE velokv_core GTest::gtest_main Threads::Threads)
  target_compile_options(velokv_tests PRIVATE ${VELOKV_WARNINGS})

  include(GoogleTest)
  gtest_discover_tests(velokv_tests DISCOVERY_TIMEOUT 30)

  # Run through bash explicitly so the test does not depend on the executable
  # bit, which is not preserved on Windows checkouts.
  add_test(NAME integration
           COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.sh $<TARGET_FILE:velokv>)
  set_tests_properties(integration PROPERTIES TIMEOUT 120)
endif()
