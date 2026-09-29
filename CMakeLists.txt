cmake_minimum_required(VERSION 3.16)
project(system_monitor LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Use the extraction timestamp for fetched archives so a changed URL rebuilds.
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

# Header-only dependencies are fetched at configure time so a fresh clone needs
# nothing but a compiler and CMake.
FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
)
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
)

set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(httplib nlohmann_json)

find_package(Threads REQUIRED)

# The monitoring code is a library so that the tests can link against it
# without pulling in the HTTP server. It deliberately knows nothing about JSON.
add_library(sysmon_core STATIC
    src/ProcFs.cpp
    src/CpuMonitor.cpp
    src/MemoryMonitor.cpp
    src/DiskMonitor.cpp
    src/ProcessMonitor.cpp
    src/SystemMonitor.cpp
)
target_include_directories(sysmon_core PUBLIC include)
target_link_libraries(sysmon_core PUBLIC Threads::Threads)

add_executable(system_monitor src/main.cpp)
target_link_libraries(system_monitor PRIVATE
    sysmon_core
    httplib::httplib
    nlohmann_json::nlohmann_json
)

foreach(target sysmon_core system_monitor)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

option(SYSMON_BUILD_TESTS "Build the unit tests" ON)

if(SYSMON_BUILD_TESTS)
    enable_testing()
    FetchContent_MakeAvailable(doctest)

    add_executable(sysmon_tests tests/test_parsing.cpp)
    target_link_libraries(sysmon_tests PRIVATE sysmon_core doctest::doctest)
    target_compile_options(sysmon_tests PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME parsing COMMAND sysmon_tests)
endif()
