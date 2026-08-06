# SPDX-License-Identifier: Apache-2.0
# fastcache-cc — the sccache-style compiler launcher, plus its unit tests.
#
# The launcher only needs the path-canonicalization + compile-value codec (and
# CRC32C for the key), which have no third-party dependencies. We compile those
# few FastCache sources directly into the tool rather than linking the whole
# FastCache static library, so the launcher pulls in NO vcpkg deps (zstd, lz4,
# yaml-cpp). That keeps it a small, self-contained binary and lets it link the
# CRT statically without a runtime-library mismatch against vcpkg objects.

set(_fc_cc_core
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/PathCanon.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/CompileValue.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Core/Crc32c.cpp")

# Fully static, copy-anywhere binary: statically link the MSVC C++ runtime so
# the launcher depends only on always-present Windows system DLLs (WS2_32,
# KERNEL32) and needs no VC++ redistributable on build agents. On by default;
# turn off to use the shared runtime (smaller binary, needs the redist).
option(FASTCACHE_STATIC_LAUNCHER "Statically link the CRT into fastcache-cc" ON)

add_executable(fastcache-cc
    main.cpp
    CmdLine.cpp
    CacheKey.cpp
    DirectManifest.cpp
    Stats.cpp
    ${_fc_cc_core}
)
target_include_directories(fastcache-cc PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(fastcache-cc PRIVATE
    FASTCACHE_CC_VERSION="${FASTCACHED_VERSION_STRING}")

if(FASTCACHE_STATIC_LAUNCHER AND MSVC)
    # CMP0091: MSVC_RUNTIME_LIBRARY selects /MT(d) vs /MD(d). Static so the CRT
    # is baked in; the generator expression keeps the Debug variant (/MTd) for
    # Debug configs.
    set_property(TARGET fastcache-cc PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Unit tests for the reusable launcher pieces (parsing + keying). These link the
# FastCache library for convenience (test binary, not shipped) so they can reach
# any test helpers; the shipped launcher above stays dependency-light.
if(FASTCACHED_BUILD_TESTS)
    add_executable(fastcache-cc-tests
        test_main.cpp
        CmdLine.cpp
        CmdLine_test.cpp
        CacheKey.cpp
        CacheKey_test.cpp
        DirectManifest.cpp
        DirectManifest_test.cpp
        ${_fc_cc_core}
    )
    target_include_directories(fastcache-cc-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(fastcache-cc-tests PRIVATE Catch2::Catch2)
    include(Catch)
    catch_discover_tests(fastcache-cc-tests)
endif()
