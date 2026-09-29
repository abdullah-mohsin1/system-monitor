cmake_minimum_required(VERSION 3.16)
project(system_monitor LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
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

set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(httplib nlohmann_json)

find_package(Threads REQUIRED)

add_executable(system_monitor
    src/main.cpp
)
target_include_directories(system_monitor PRIVATE include)
target_link_libraries(system_monitor PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads
)
target_compile_options(system_monitor PRIVATE -Wall -Wextra -Wpedantic)
