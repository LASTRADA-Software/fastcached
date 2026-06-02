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
set(CMAKE_COLOR_DIAGNOSTICS ON)

# ----------------------------------------------------------------------------
# Project-wide options

option(FASTCACHED_BUILD_TESTS "Build unit tests" ON)
option(USE_SCCACHE "Use sccache as the compiler launcher when the binary is on PATH" ON)

# ----------------------------------------------------------------------------
# Compiler caching (sccache). Loaded before any compiler invocation so that
# CPM-fetched dependencies also get cached. cache.cmake silently no-ops when
# sccache is not installed or USE_SCCACHE=OFF.

include(cache)

# Default install/runtime/library output directories.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/target")
endif()

# ----------------------------------------------------------------------------
# Dependencies via CPM (tries find_package first).
#
# Fetched BEFORE the pedantic/sanitizer/clang-tidy includes so the global
# add_compile_options() / CMAKE_CXX_CLANG_TIDY they install only apply to our
# own targets (defined later in this directory), not to third-party code where
# -Werror -Wconversion is not our problem to fix.

set(CPM_USE_LOCAL_PACKAGES ON CACHE BOOL "Prefer system-installed packages over CPM downloads")
include(cmake/CPM.cmake)

# yaml-cpp 0.8.0 declares cmake_minimum_required(VERSION 3.4) which CMake 4.x
# refuses outright. Inject the policy-version compatibility shim so the fetch
# configures on modern toolchains. This is scoped to yaml-cpp only.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
CPMAddPackage(
    NAME yaml-cpp
    GITHUB_REPOSITORY jbeder/yaml-cpp
    GIT_TAG 0.8.0
    OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_BUILD_SHARED_LIBS OFF"
        "YAML_CPP_INSTALL OFF"
)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)

if(FASTCACHED_BUILD_TESTS)
    CPMAddPackage(
        NAME Catch2
        GITHUB_REPOSITORY catchorg/Catch2
        VERSION 3.6.0
    )

    # Locate the directory shipping Catch.cmake (providing catch_discover_tests)
    # and put it on CMAKE_MODULE_PATH so every test subdirectory can simply
    # include(Catch). Catch2 reaches us by one of two routes:
    #   * CPM source build  -> helpers live in ${Catch2_SOURCE_DIR}/extras
    #   * system find_package -> helpers live next to Catch2Config.cmake,
    #                            i.e. in ${Catch2_DIR}
    # CPM_USE_LOCAL_PACKAGES makes the latter the default whenever a system
    # Catch2 is installed, so both routes must be handled.
    if(Catch2_SOURCE_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    elseif(Catch2_DIR)
        list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
    endif()
endif()

# ----------------------------------------------------------------------------
# Toolchain helpers (warning levels, sanitizers, clang-tidy).

include(PedanticCompiler)
include(Sanitizers)
include(ClangTidy)

# ----------------------------------------------------------------------------
# Test wiring is enabled early so all subdirectories (CowTree, FastCache,
# src/tests) can register their CTest test executables via
# catch_discover_tests().

if(FASTCACHED_BUILD_TESTS)
    enable_testing()
endif()

# ----------------------------------------------------------------------------
# Sibling libraries (no dependency on FastCache).

add_subdirectory(src/CowTree)

# ----------------------------------------------------------------------------
# Library target: all FastCache code lives here (so both the executable and
# the test binary link it).

add_library(FastCache STATIC)
target_include_directories(FastCache PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_compile_features(FastCache PUBLIC cxx_std_23)
target_link_libraries(FastCache PUBLIC yaml-cpp::yaml-cpp)
target_link_libraries(FastCache PUBLIC CowTree)

# Substitute the version triple/string into Core/Version.hpp at configure
# time. The generated header lives under the build dir so it never gets
# checked in.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/FastCache/Core/Version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/FastCache/Core/Version.hpp"
    @ONLY
)

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
    add_subdirectory(src/tests)
endif()
