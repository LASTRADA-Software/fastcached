# SPDX-License-Identifier: Apache-2.0
# Catch2 test binary. Picks up every *_test.cpp adjacent to source files in
# src/FastCache so adding a test file is a one-step operation: drop it next
# to the implementation and re-configure.

file(GLOB_RECURSE FASTCACHE_TEST_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/FastCache/*_test.cpp"
)

add_executable(FastCacheTest
    test_main.cpp
    ${FASTCACHE_TEST_SOURCES}
)

target_link_libraries(FastCacheTest
    PRIVATE
        FastCache
        Catch2::Catch2
)

# Absolute path to checked-in test fixtures (e.g. the throwaway TLS cert/key),
# so tests can open them regardless of the working directory.
target_compile_definitions(FastCacheTest
    PRIVATE
        FASTCACHED_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata"
)

# Catch.cmake (catch_discover_tests) is placed on CMAKE_MODULE_PATH by the
# top-level CMakeLists, regardless of whether Catch2 came from CPM source or a
# system find_package.
include(Catch)
catch_discover_tests(FastCacheTest)

# ---------------------------------------------------------------------------
# Repository hygiene. Fails when a path that must never be committed is tracked
# by git. Today that is version.txt, whose committed presence outranked every git
# tag and pinned every build, every wire banner and every package to whatever it
# said. scripts/check-repository-hygiene.cmake carries the table of forbidden
# paths and the reason each one is forbidden, and prints the reason on failure.
#
# Deliberately NOT labelled "smoke": it starts no daemon, opens no socket and
# needs no compiler, so it belongs in the default `ctest` set and must survive
# `ctest -LE smoke`. It finishes in milliseconds.
#
# GIT_EXECUTABLE is located by cmake/Version.cmake and passed in rather than
# looked up again, so the check and the version resolution cannot disagree about
# which git they mean.
add_test(
    NAME "repository-hygiene"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-repository-hygiene.cmake"
)
# An exported source tarball has no git index to inspect, and the script reports
# that by printing `SKIP: ...` and exiting 0. SKIP_REGULAR_EXPRESSION and not the
# SKIP_RETURN_CODE 77 the drivers below use, because a `cmake -P` script cannot
# choose its own exit code before CMake 3.29 (cmake_language(EXIT)) while this
# project supports 3.28 — a SKIP_RETURN_CODE it could never return would be
# decoration. See the script header. TIMEOUT 60 rather than CTest's 1500s
# default: a wedged git should report in a minute, not stall the suite.
set_tests_properties("repository-hygiene" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 60
)

# ---------------------------------------------------------------------------
# Compiler-cache auto-install fallback. Configures the tiny fixture project in
# compile-cache-fixture/ once per way the fetch can be declined, and requires
# each one to end in a status message and a clean fall-through.
#
# What it defends is the property that makes cmake/CompileCache.cmake safe to
# vendor: it is copied into repositories that never asked for fastcached, so a
# cache it cannot download has to cost them a slower build and nothing else. A
# configure that aborts because GitHub was unreachable would be a far worse
# failure than the missing cache it is reporting.
#
# Not labelled "smoke" for the same reason as repository-hygiene: it starts no
# daemon and opens no listening socket, so it belongs in the default `ctest`
# set. It does invoke the compiler (a project has to configure), which is why
# it is given more than that test's minute.
#
# The compiler, generator and make program are passed in rather than rediscovered
# so the fixture is configured the same way this build was, and the script's
# sandbox is the only thing that differs.
add_test(
    NAME "compile-cache-autoinstall"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-autoinstall"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-autoinstall.cmake"
)
# Windows and a host without uname both report by printing `SKIP: ` and exiting
# 0 — see the script header for why the sandbox it needs is POSIX-only, and
# repository-hygiene above for why a `cmake -P` script cannot return 77.
set_tests_properties("compile-cache-autoinstall" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# The other half: that it can actually install one. Serves a synthetic release
# over file:// through FASTCACHE_AUTO_INSTALL_DOWNLOAD_BASE — the same seam an
# organisation uses to install from an internal mirror — so the happy path is
# covered without the network, and without spending the GitHub API budget that
# a fleet of CI runners behind one address has to share.
add_test(
    NAME "compile-cache-install"
    COMMAND ${CMAKE_COMMAND}
        "-DFASTCACHED_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        "-DFASTCACHED_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/compile-cache-install"
        "-DFASTCACHED_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DFASTCACHED_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DFASTCACHED_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_SOURCE_DIR}/scripts/check-compile-cache-install.cmake"
)
set_tests_properties("compile-cache-install" PROPERTIES
    LABELS "hygiene"
    SKIP_REGULAR_EXPRESSION "SKIP: "
    TIMEOUT 600
)

# ---------------------------------------------------------------------------
# sccache smoke tests. Registered only when an sccache binary is present
# (SCCACHE is the find_program result from cmake/CompileCache.cmake). Each test starts
# the freshly-built fastcached, points sccache at it over one wire protocol,
# and asserts a cache hit — the same compile-twice check the CI smoke jobs run,
# via the same scripts/sccache-smoke driver.
#
# They start a real server and invoke the real compiler, so they are slow and
# environment-dependent: the "smoke" label lets you select (ctest -L smoke) or
# skip (ctest -LE smoke) them, and a missing runtime prerequisite exits 77 to
# report as skipped rather than failed.

# ---------------------------------------------------------------------------
# TLS smoke test. Registered only in TLS-enabled builds. Starts the freshly
# built fastcached with the checked-in self-signed cert, connects a real TLS
# client, and asserts a RESP PING is answered +PONG over the encrypted channel
# — end-to-end coverage of the TlsSocket handshake/pump. A missing runtime
# prerequisite exits 77 (reported as skipped).
if(FASTCACHED_ENABLE_TLS)
    if(WIN32)
        add_test(
            NAME "tls-smoke"
            COMMAND pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/tls-smoke.ps1"
                -Fastcached "$<TARGET_FILE:fastcached>"
                -Cert "${CMAKE_SOURCE_DIR}/testdata/tls/server.crt"
                -Key "${CMAKE_SOURCE_DIR}/testdata/tls/server.key"
        )
    else()
        add_test(
            NAME "tls-smoke"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/tls-smoke.sh"
                --fastcached "$<TARGET_FILE:fastcached>"
                --cert "${CMAKE_SOURCE_DIR}/testdata/tls/server.crt"
                --key "${CMAKE_SOURCE_DIR}/testdata/tls/server.key"
        )
    endif()
    set_tests_properties("tls-smoke" PROPERTIES LABELS "smoke" SKIP_RETURN_CODE 77)
endif()

if(SCCACHE)
    set(FASTCACHED_SMOKE_PORT "11611" CACHE STRING "TCP port the sccache smoke tests bind fastcached to")

    if(WIN32)
        set(_smoke_driver pwsh -NoProfile -File "${CMAKE_SOURCE_DIR}/scripts/sccache-smoke.ps1")
    else()
        set(_smoke_driver bash "${CMAKE_SOURCE_DIR}/scripts/sccache-smoke.sh")
    endif()

    foreach(_proto IN ITEMS memcached redis)
        add_test(
            NAME "sccache-smoke-${_proto}"
            COMMAND ${_smoke_driver}
                --fastcached "$<TARGET_FILE:fastcached>"
                --protocol ${_proto}
                --port ${FASTCACHED_SMOKE_PORT}
                --compiler "${CMAKE_CXX_COMPILER}"
        )
        # Both tests bind the same port and drive one shared sccache server, so
        # they must not run concurrently with each other.
        set_tests_properties("sccache-smoke-${_proto}" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
        )
    endforeach()
else()
    message(STATUS "sccache not found — skipping sccache smoke tests")
endif()
