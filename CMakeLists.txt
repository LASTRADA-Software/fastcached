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

include(GNUInstallDirs)

# Lay the install tree out for a distro package: the payload is rooted at /
# instead of /usr, because the systemd units and the /etc config sit at fixed
# absolute locations that cannot be rebased onto a /usr prefix (systemd does
# not read config from /usr/etc). See cmake/Packaging.cmake.
#
# This is OFF by default, including on Linux, so that the ordinary
# `cmake --install --prefix /usr/local` keeps putting the daemon at
# /usr/local/bin/fastcached. Defaulting it ON rebased every developer install
# onto <prefix>/usr/bin, which is on nobody's PATH — a successful install that
# left `fastcached` reporting "command not found".
#
# Turn it ON only to build packages:
#   cmake -DFASTCACHED_PACKAGE_ROOT_PREFIX=ON ... && cpack -G "DEB;RPM"
# The CI packaging job does exactly that; see .github/workflows/build.yml.
option(FASTCACHED_PACKAGE_ROOT_PREFIX "Lay out the install tree for .deb/.rpm packaging" OFF)

# Install destinations for the relocatable artifacts (binaries, docs). With the
# prefix moved to /, the ordinary FHS paths have to spell `usr/` themselves, or
# the binaries would land in /bin and the docs in /share/doc. Outside packaging
# these stay the plain GNUInstallDirs values.
if(FASTCACHED_PACKAGE_ROOT_PREFIX)
    set(FASTCACHED_INSTALL_BINDIR "usr/${CMAKE_INSTALL_BINDIR}")
    set(FASTCACHED_INSTALL_DOCDIR "usr/${CMAKE_INSTALL_DOCDIR}")
else()
    set(FASTCACHED_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
    set(FASTCACHED_INSTALL_DOCDIR "${CMAKE_INSTALL_DOCDIR}")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# ----------------------------------------------------------------------------
# Project-wide options

option(FASTCACHED_BUILD_TESTS "Build unit tests" ON)
option(USE_COMPILER_CACHE "Use a compiler-cache launcher when one is available (fastcache-cc when configured, else sccache)" ON)

# TLS is opt-in and off by default so the standard build stays dependency-free
# (mirroring TRACY_ENABLE). When on, the build links OpenSSL and our code keys
# off the FC_TLS_ENABLED compile definition; when off, no OpenSSL header is
# included and the TLS sources are not compiled. On Windows the OpenSSL package
# is pulled in via the vcpkg manifest's "tls" feature (see vcpkg.json).
option(FASTCACHED_ENABLE_TLS "Enable TLS support (requires OpenSSL)" OFF)

# On-disk value compression is ON by default: the standard build ships zstd +
# lz4 and the runtime default codec is zstd (admins opt out with
# `--compression none`). When explicitly built OFF, no compression library is
# fetched or linked, the codec table degrades to identity-only, and selecting
# zstd/lz4 from config becomes a clean startup error. Our code keys off the
# FC_COMPRESSION_ENABLED compile definition (set in src/FastCache/CMakeLists.txt).
option(FASTCACHED_ENABLE_COMPRESSION "Enable on-disk value compression (zstd + lz4)" ON)

# Tracy profiler. TRACY_ENABLE is Tracy's own gate; reusing the name means the
# fetched client library actually emits instrumentation rather than its no-op
# stub. Our code keys off the separate FC_TRACY_ENABLED compile definition
# (set below) so Core/Profiling.hpp stays decoupled from Tracy's macro surface
# and pulls in no Tracy header when the profiler is off.
option(TRACY_ENABLE "Enable Tracy profiler instrumentation (fetches & links the Tracy client)" OFF)
option(TRACY_ON_DEMAND "Tracy: only collect data while a profiler is connected" ON)

# ----------------------------------------------------------------------------
# Compiler caching. Loaded before any compiler invocation so that CPM-fetched
# dependencies also get cached. CompileCache.cmake prefers our own fastcache-cc
# launcher (when installed and FASTCACHE_ADDR names a daemon), falls back to
# sccache, and silently no-ops when neither is usable or USE_COMPILER_CACHE=OFF.

include(CompileCache)

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

# Tracy is third-party, so (like yaml-cpp/Catch2) it is fetched BEFORE the
# pedantic/clang-tidy includes below — the global -Werror and clang-tidy hooks
# must not apply to its sources. We forward TRACY_ENABLE/TRACY_ON_DEMAND so the
# client emits instrumentation; TRACY_ON_DEMAND keeps a long-running daemon from
# buffering events (and growing memory) until a profiler actually connects.
if(TRACY_ENABLE)
    CPMAddPackage(
        NAME tracy
        GITHUB_REPOSITORY wolfpld/tracy
        GIT_TAG v0.11.1
        OPTIONS
            "TRACY_ENABLE ON"
            "TRACY_ON_DEMAND ${TRACY_ON_DEMAND}"
            "TRACY_STATIC ON"
    )
endif()

# OpenSSL for the optional TLS support. Located before the pedantic/clang-tidy
# includes so its imported targets carry no -Werror expectations of ours. The
# REQUIRED here means a misconfigured TLS build fails fast at configure time
# rather than at link time.
if(FASTCACHED_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

# zstd + lz4 for on-disk value compression. Fetched (like yaml-cpp/Tracy)
# BEFORE the pedantic/clang-tidy includes so the global -Werror -Wconversion
# and clang-tidy hooks never apply to their third-party sources. CPM tries a
# system package first (CPM_USE_LOCAL_PACKAGES), otherwise fetches the source.
# Both build static, programs/tests off, to keep the dependency footprint small.
if(FASTCACHED_ENABLE_COMPRESSION)
    CPMAddPackage(
        NAME zstd
        GITHUB_REPOSITORY facebook/zstd
        VERSION 1.5.6
        SOURCE_SUBDIR build/cmake
        OPTIONS
            "ZSTD_BUILD_PROGRAMS OFF"
            "ZSTD_BUILD_TESTS OFF"
            "ZSTD_BUILD_SHARED OFF"
            "ZSTD_BUILD_STATIC ON"
    )
    CPMAddPackage(
        NAME lz4
        GITHUB_REPOSITORY lz4/lz4
        VERSION 1.10.0
        SOURCE_SUBDIR build/cmake
        OPTIONS
            "LZ4_BUILD_CLI OFF"
            "LZ4_BUILD_LEGACY_LZ4C OFF"
            "BUILD_SHARED_LIBS OFF"
            "BUILD_STATIC_LIBS ON"
    )
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

# Tracy is linked PUBLIC so FC_TRACY_ENABLED and the Tracy include dirs
# propagate transitively to the fastcached executable (thread naming in
# main.cpp) and to the FastCacheTest binary (so Profiling_test sees the same
# mode). When TRACY_ENABLE is OFF nothing is linked or defined and every
# Core/Profiling.hpp macro compiles to (void)0 — a strictly zero-cost build.
if(TRACY_ENABLE)
    target_link_libraries(FastCache PUBLIC Tracy::TracyClient)
    target_compile_definitions(FastCache PUBLIC FC_TRACY_ENABLED=1)
endif()

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
# Executables. Every binary lives under src/apps/<name>/ and declares its own
# target and install rule there; src/apps/CMakeLists.txt holds the app table
# that gates each one. The shipped binaries are fastcached and fastcache-cc;
# compile-cache-testclient is test infrastructure and is off by default.

add_subdirectory(src/apps)

# ----------------------------------------------------------------------------
# Installation of the project docs, under the standard GNUInstallDirs prefix
# (share/doc/fastcached/). Each executable installs itself into bin/ from its
# own subdirectory. The FastCache/CowTree static libraries and headers are
# intentionally not installed: they PUBLIC-link an un-installed CPM yaml-cpp,
# so exporting a consumable SDK is out of scope.

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${FASTCACHED_INSTALL_DOCDIR}"
    COMPONENT Runtime
)

# ----------------------------------------------------------------------------
# Service-integration assets (systemd units, sysusers/tmpfiles declarations,
# the example config, the WiX service fragment) and the CPack configuration
# that wraps everything installed above into .deb / .rpm / .msi.
#
# Order matters twice over: packaging/ exports the variables Packaging.cmake
# consumes, and CPack only sees install() rules declared before include(CPack)
# — so this stays at the end, after every install() in the project.

add_subdirectory(packaging)
include(Packaging)

# ----------------------------------------------------------------------------
# Tests.

if(FASTCACHED_BUILD_TESTS)
    add_subdirectory(src/tests)
endif()
