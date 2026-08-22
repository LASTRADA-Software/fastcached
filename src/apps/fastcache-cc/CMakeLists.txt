# SPDX-License-Identifier: Apache-2.0
# fastcache-cc — the sccache-style compiler launcher, plus its unit tests.
#
# The launcher only needs the path-canonicalization + compile-value codec, the
# shared usage renderer, and the two leaf platform probes -- none of which have
# third-party dependencies. We compile those few FastCache sources directly into
# the tool rather than linking the whole FastCache static library, so the
# launcher pulls in NO vcpkg deps (zstd, lz4, yaml-cpp). That keeps it a small,
# self-contained binary and lets it link the CRT statically without a
# runtime-library mismatch against vcpkg objects.
#
# Cli/UsageDoc.cpp is what makes `fastcache-cc --help` render and colorize
# exactly like `fastcached --help`; Platform/Terminal.cpp decides whether to,
# and Platform/Environment.cpp is the one place the environment is read, so the
# launcher no longer needs its own copy of that dance.
#
# Note what is NOT here: Core/Crc32c.cpp. It was carried for the cache key, and
# the key no longer uses it -- Core/MurmurHash3.hpp replaced the four salted
# CRC32C runs that gave a 128-bit-wide key 32 bits of strength (issue #63), and
# it is header-only, so the component that replaced a compiled row costs none.
# The remaining CRC32C users are all daemon-side page and record checksums.

set(_fc_cc_core
    "${CMAKE_SOURCE_DIR}/src/FastCache/Cli/UsageDoc.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/PathCanon.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/CompileValue.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Core/Compression.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Platform/Environment.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Platform/Terminal.cpp")

# Core/Compression.cpp is the one row here that is NOT dependency-free, and it is a
# deliberate exception to the rule the comment above states.
#
# Distributed compilation sends a preprocessed translation unit over the network --
# several megabytes for an ordinary C++ TU, on the hot path of a parallel build --
# and shipping that uncompressed is the difference between distribution being worth
# switching on and not. The codec table is also already the daemon's, so the
# alternative was a second compressor in the launcher, which is worse: two
# implementations of one wire format, one of them untested against the other.
#
# What it costs, stated so nobody rediscovers it: the launcher's build now needs
# zstd and lz4, which are CPM-fetched rather than vcpkg, so MSVC_RUNTIME_LIBRARY
# still propagates and the static-CRT link is unaffected. It does mean the launcher
# no longer builds without a network on a cold CPM cache, and it inherits the macOS
# constraint the packaging job already carries: CPM_USE_LOCAL_PACKAGES=OFF, or a
# Homebrew shared zstd gets linked and `otool -L` fails the "nothing outside
# /usr/lib" assertion.
#
# FASTCACHED_ENABLE_COMPRESSION=OFF stays a working configuration: the codec table
# compiles without either library, Identity is always available, and the wire
# negotiation falls back to it -- so such a launcher still dispatches, just without
# compressing. That is asserted, not assumed.
if(FASTCACHED_ENABLE_COMPRESSION)
    if(TARGET libzstd_static)
        set(_fc_cc_zstd libzstd_static)
    elseif(TARGET zstd::libzstd_static)
        set(_fc_cc_zstd zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        set(_fc_cc_zstd zstd::libzstd_shared)
    endif()
    if(TARGET lz4_static)
        set(_fc_cc_lz4 lz4_static)
    elseif(TARGET LZ4::lz4_static)
        set(_fc_cc_lz4 LZ4::lz4_static)
    elseif(TARGET lz4::lz4)
        set(_fc_cc_lz4 lz4::lz4)
    endif()
endif()

# The launcher's own reusable pieces. Process spawning and the TCP client are
# reached through interfaces (IProcessRunner / ITcpClient) so the flow logic
# stays platform-free and testable against fakes on every host.
# CacheProtocol.cpp is here rather than in main.cpp on purpose: main.cpp is in
# no test target, so while the wire framing lived there it had no unit coverage
# at all -- the only thing exercising it was the end-to-end script, which needs
# a daemon and a compiler. As its own translation unit it can be driven against
# a scripted ITcpClient, which is what lets "a rejected version is reported
# distinctly from a miss" be a test rather than a hope.
set(_fc_cc_sources
    CmdLine.cpp
    CompileJob.cpp
    LauncherCli.cpp
    CacheKey.cpp
    CacheProtocol.cpp
    Dispatch.cpp
    DependencyOutput.cpp
    DependencyProbe.cpp
    DirectManifest.cpp
    PathResolve.cpp
    ReplayGuard.cpp
    RootReconciler.cpp
    Stats.cpp
    ToolchainFingerprint.cpp
    ProcessRunner.cpp
    TcpClient.cpp
    WorkerProtocol.cpp)

# Fully static, copy-anywhere binary: statically link the MSVC C++ runtime so
# the launcher depends only on always-present Windows system DLLs (WS2_32,
# KERNEL32) and needs no VC++ redistributable on build agents. On by default;
# turn off to use the shared runtime (smaller binary, needs the redist).
option(FASTCACHED_STATIC_LAUNCHER "Statically link the CRT into fastcache-cc" ON)

add_executable(fastcache-cc
    main.cpp
    ${_fc_cc_sources}
    ${_fc_cc_core}
)
target_include_directories(fastcache-cc PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(fastcache-cc PRIVATE
    FASTCACHE_CC_VERSION="${FASTCACHED_VERSION_STRING}")
if(FASTCACHED_ENABLE_COMPRESSION AND _fc_cc_zstd AND _fc_cc_lz4)
    target_link_libraries(fastcache-cc PRIVATE ${_fc_cc_zstd} ${_fc_cc_lz4})
    target_compile_definitions(fastcache-cc PRIVATE FC_COMPRESSION_ENABLED=1)
endif()

if(WIN32)
    target_link_libraries(fastcache-cc PRIVATE ws2_32)
endif()

if(FASTCACHED_STATIC_LAUNCHER AND MSVC)
    # CMP0091: MSVC_RUNTIME_LIBRARY selects /MT(d) vs /MD(d). Static so the CRT
    # is baked in; the generator expression keeps the Debug variant (/MTd) for
    # Debug configs.
    set_property(TARGET fastcache-cc PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

install(TARGETS fastcache-cc
    RUNTIME DESTINATION "${FASTCACHED_INSTALL_BINDIR}"
    COMPONENT Runtime
)

# Unit tests for the reusable launcher pieces (parsing, keying, statistics, and
# the platform seams). Like the launcher itself these avoid the FastCache
# library, so the test binary stays dependency-light — Catch2 is the only extra.
if(FASTCACHED_BUILD_TESTS)
    add_executable(fastcache-cc-tests
        test_main.cpp
        ${_fc_cc_sources}
        CmdLine_test.cpp
        CompileJob_test.cpp
        LauncherCli_test.cpp
        CacheKey_test.cpp
        CacheProtocol_test.cpp
        Dispatch_test.cpp
        DependencyOutput_test.cpp
        DependencyProbe_test.cpp
        DirectManifest_test.cpp
        PathResolve_test.cpp
        ReplayGuard_test.cpp
        RootReconciler_test.cpp
        ProcessRunner_test.cpp
        Stats_test.cpp
        ToolchainFingerprint_test.cpp
        TcpClient_test.cpp
        WorkerProtocol_test.cpp
        ${_fc_cc_core}
    )
    target_include_directories(fastcache-cc-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_link_libraries(fastcache-cc-tests PRIVATE Catch2::Catch2)
    if(FASTCACHED_ENABLE_COMPRESSION AND _fc_cc_zstd AND _fc_cc_lz4)
        target_link_libraries(fastcache-cc-tests PRIVATE ${_fc_cc_zstd} ${_fc_cc_lz4})
        target_compile_definitions(fastcache-cc-tests PRIVATE FC_COMPRESSION_ENABLED=1)
    endif()
    if(WIN32)
        target_link_libraries(fastcache-cc-tests PRIVATE ws2_32)
    endif()
    include(Catch)
    catch_discover_tests(fastcache-cc-tests)

    # ---------------------------------------------------------------------
    # End-to-end compile-cache test. Starts a real daemon, drives real
    # compiles through the launcher, and asserts miss/hit, byte-identical
    # object reproduction, and cross-depth portability — the properties unit
    # tests cannot cover because they need a compiler and a server.
    #
    # Labelled "smoke" so it can be selected (ctest -L smoke) or skipped
    # (ctest -LE smoke); a missing prerequisite exits 77 and reports as
    # skipped rather than failed. Both platforms assert the same contract.
    #
    # The test drives a real daemon, so it only exists when the daemon is being
    # built: the app table makes each executable independently gateable, and an
    # unguarded $<TARGET_FILE:fastcached> would break configuration outright
    # under -DFASTCACHED_BUILD_DAEMON=OFF.
    if(NOT TARGET fastcached)
        message(STATUS "fastcache-cc: skipping compile-cache-e2e test (fastcached not built)")
    elseif(WIN32)
        add_test(
            NAME "compile-cache-e2e"
            COMMAND pwsh -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/run-launcher-e2e.ps1"
                -Fastcached "$<TARGET_FILE:fastcached>"
                -Launcher "$<TARGET_FILE:fastcache-cc>"
        )
    else()
        add_test(
            NAME "compile-cache-e2e"
            COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/compile-cache-e2e.sh"
                --fastcached "$<TARGET_FILE:fastcached>"
                --launcher "$<TARGET_FILE:fastcache-cc>"
                --compiler "${CMAKE_CXX_COMPILER}"
        )
    endif()
    if(TARGET fastcached)
        # Binds a fixed port and spawns a daemon, so it must not run concurrently
        # with the other server-starting smoke tests.
        set_tests_properties("compile-cache-e2e" PROPERTIES
            LABELS "smoke"
            SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE
        )
    endif()
endif()
