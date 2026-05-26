# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(Version)
GetVersionInformation(FASTCACHED_VERSION FASTCACHED_VERSION_STRING)

project(fastcached
    VERSION "${FASTCACHED_VERSION}"
    DESCRIPTION "Fast Cache Daemon — platform-independent memcached/Redis server"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----------------------------------------------------------------------------
# Project-wide options

option(FASTCACHED_BUILD_TESTS "Build unit tests" ON)

# Default install/runtime/library output directories.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()

# ----------------------------------------------------------------------------
# Toolchain helpers (warning levels, sanitizers, clang-tidy).

include(PedanticCompiler)
include(Sanitizers)
include(ClangTidy)

# ----------------------------------------------------------------------------
# Dependencies via CPM (tries find_package first).

set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Prefer system-installed packages over CPM downloads")
include(cmake/CPM.cmake)

CPMAddPackage(
    NAME yaml-cpp
    GITHUB_REPOSITORY jbeder/yaml-cpp
    VERSION 0.8.0
    OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_BUILD_SHARED_LIBS OFF"
        "YAML_CPP_INSTALL OFF"
)

if(FASTCACHED_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.6.0
    )
endif()

# ----------------------------------------------------------------------------
# Library target: all FastCache code lives here (so both the executable and
# the test binary link it).

add_library(FastCache STATIC)
target_include_directories(FastCache PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(FastCache PUBLIC cxx_std_23)
target_link_libraries(FastCache PUBLIC yaml-cpp::yaml-cpp)

if(WIN32)
    target_link_libraries(FastCache PUBLIC ws2_32)
    target_compile_definitions(FastCache PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
endif()

add_subdirectory(src/FastCache)

# ----------------------------------------------------------------------------
# Executable target.

add_executable(fastcached src/fastcached/main.cpp)
target_link_libraries(fastcached PRIVATE FastCache)

# ----------------------------------------------------------------------------
# Tests.

if(FASTCACHED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(src/tests)
endif()
