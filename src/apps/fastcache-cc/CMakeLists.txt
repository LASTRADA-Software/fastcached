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
# launcher no longer needs its own copy of that dance. Platform/Registry.cpp is
# there for the same reason one layer over: locating an MSVC toolchain without an
# inherited `INCLUDE` means reading the Windows SDK's installed roots, and a
# second copy of the two-call size dance and the WOW64 view is a second chance to
# get either wrong. It compiles to nothing on POSIX, so it costs that build a
# translation unit and no dependency.
#
# Note what is NOT here: Core/Crc32c.cpp. It was carried for the cache key, and
# the key no longer uses it -- Core/MurmurHash3.hpp replaced the four salted
# CRC32C runs that gave a 128-bit-wide key 32 bits of strength (issue #63), and
# it is header-only, so the component that replaced a compiled row costs none.
# The remaining CRC32C users are all daemon-side page and record checksums.

set(_fc_cc_core
    "${CMAKE_SOURCE_DIR}/src/FastCache/Async/DeadlineTimer.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Async/EpollReactor.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Async/InterruptibleSleep.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Async/IocpReactor.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Async/KqueueReactor.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Cli/UsageDoc.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/PathCanon.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/CompileCache/CompileValue.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Core/Compression.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/SocketAddress.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/BlockingSocket.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/BlockingConnector.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/ConnectFlow.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/EpollConnector.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/EpollSocket.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/IocpConnector.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/IocpSocket.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/KqueueConnector.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/KqueueSocket.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/TcpClient.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Net/ThreadedAddressResolver.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Platform/Environment.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Platform/Registry.cpp"
    "${CMAKE_SOURCE_DIR}/src/FastCache/Platform/Terminal.cpp")

# The Net rows are the launcher's TCP client -- the blocking pair it dials with,
# plus the reactor sockets and connectors that share its code -- and they are here
# rather than in a copy of their own, because a copy is what went wrong: this tree had
# three implementations of "dial, then send and receive whole messages", and the
# one nobody compiled (compile-cache-testclient) was IPv4-literal-only, unbounded
# and unprotected against SIGPIPE. See src/FastCache/Net/TcpClient.hpp.
#
# They cost the launcher nothing it was avoiding. The rule this file exists to
# protect is that fastcache-cc does not LINK FastCache -- so it stays free of
# yaml-cpp, OpenSSL and the reactor, and can link the CRT statically -- and these
# four reach only Net/NetError.hpp, Async/Task.hpp and Core/Profiling.hpp,
# all header-only and all std-only. ws2_32 was already linked below.

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
# zstd and lz4, which are CPM-fetched rather than vcpkg -- and they are built with
# CMake's default MSVC runtime, which the static-CRT link below has to work around
# in Debug (see the MSVC_RUNTIME_LIBRARY property for what it costs and why the
# obvious fix is unavailable). It does mean the launcher
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

# The launcher's own reusable pieces. Process spawning is reached through an
# interface (IProcessRunner) so the flow logic stays platform-free and testable
# against fakes on every host; the TCP side reaches the library's ISocket seam
# through the Net rows above.
# CacheProtocol.cpp is here rather than in main.cpp on purpose: main.cpp is in
# no test target, so while the wire framing lived there it had no unit coverage
# at all -- the only thing exercising it was the end-to-end script, which needs
# a daemon and a compiler. As its own translation unit it can be driven against
# a scripted ISocket, which is what lets "a rejected version is reported
# distinctly from a miss" be a test rather than a hope.
set(_fc_cc_sources
    CmdLine.cpp
    CompileJob.cpp
    LauncherCli.cpp
    CacheKey.cpp
    CacheProtocol.cpp
    ReactorExchange.cpp
    Dispatch.cpp
    EndpointDial.cpp
    DependencyOutput.cpp
    DependencyProbe.cpp
    DirectManifest.cpp
    PathResolve.cpp
    ReplayGuard.cpp
    RootReconciler.cpp
    Stats.cpp
    ToolchainDiscovery.cpp
    ToolchainFingerprint.cpp
    ToolchainHost.cpp
    ToolchainProbe.cpp
    ProcessRunner.cpp
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

# `Net/ThreadedAddressResolver.cpp` runs `getaddrinfo` on a pool of its own, because
# it takes no timeout and a wedged resolver would otherwise stall every translation
# unit in the build. That is the one thing this launcher genuinely needs a thread
# for, and it needs the platform's thread support linked to get it.
#
# NOT a departure from the rule this file exists to protect. That rule is about not
# linking FastCache and therefore not dragging in yaml-cpp, OpenSSL or the reactor's
# transitive world; the C++ standard library's own threading support is not that
# kind of dependency, and a static launcher still links no third-party code. It is
# named explicitly rather than left to glibc folding pthread into libc since 2.34 --
# that happens to work on the distributions CI uses and is not something to rely on.
find_package(Threads REQUIRED)
target_link_libraries(fastcache-cc PRIVATE Threads::Threads)
# The reactor and its platform sockets are compiled in so the launcher can dial
# without blocking -- see Net/IConnector.hpp. They cost no new library: the
# reactor headers reach only Async/IReactor.hpp and Core/Clock.hpp, both
# header-only and std-only, and the socket sources reach only NetError.hpp,
# BlockingSocket.hpp and SocketAddress.hpp, which were already here. All six are
# listed unconditionally and guard themselves with a file-scope #if, exactly as
# src/FastCache/CMakeLists.txt lists them.
#
# WIN32_LEAN_AND_MEAN / NOMINMAX / _WIN32_WINNT are NOT optional and NOT
# inherited: they are PUBLIC on the FastCache target, and this binary
# deliberately does not link FastCache. Without them the Windows build fails
# outright, because Net/IocpSocket.hpp -> Async/IocpReactor.hpp -> <windows.h>
# pulls in winsock.h BEFORE Net/IocpSocket.cpp includes <winsock2.h> -- the
# classic redefinition. Platform/Terminal.cpp already includes <windows.h> here
# and survives only because nothing in that translation unit also wants winsock2.
if(WIN32)
    target_compile_definitions(fastcache-cc PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()


if(FASTCACHED_STATIC_LAUNCHER AND MSVC)
    # CMP0091: MSVC_RUNTIME_LIBRARY selects /MT(d) vs /MD(d). Static so the CRT is
    # baked in and the shipped launcher needs no redistributable -- it is fetched as
    # a bare binary by FASTCACHE_AUTO_INSTALL, onto machines that have no reason to
    # carry one.
    #
    # DEBUG IS THE EXCEPTION, and it is not a preference. zstd and lz4 are CPM-built
    # here and take CMake's default runtime (the DLL one), because the FastCache
    # library links the very same static libraries and the daemon's vcpkg
    # dependencies are dynamic-CRT builds -- so those two targets cannot both be
    # satisfied by one setting. In Release nothing breaks: the CRT entry points zstd
    # calls resolve out of whichever CRT the exe links. In Debug they do not, because
    # `assert()` references `__imp__wassert`, which exists ONLY in the DLL CRT, and
    # the launcher stopped linking at all the moment compression was added to it:
    #
    #     zstd_static.lib(zstd_compress.c.obj) : error LNK2001:
    #         unresolved external symbol __imp__wassert
    #
    # Windows CI builds Release only, so `cl-debug` and `clangcl-debug` -- both
    # documented in AGENT.md -- were broken for every Windows developer while every
    # job stayed green. The Debug launcher is a developer artefact and never shipped,
    # so it takes the runtime its dependencies were built with; the artefact that IS
    # shipped keeps the static CRT it has always had.
    set_property(TARGET fastcache-cc PROPERTY
        MSVC_RUNTIME_LIBRARY "$<IF:$<CONFIG:Debug>,MultiThreadedDebugDLL,MultiThreaded>")
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
        ReactorExchange_test.cpp
        # Test infrastructure, and on THIS target only: the shipped launcher must
        # not carry a test double, and `_fc_cc_core` is what it is built from.
        "${CMAKE_SOURCE_DIR}/src/FastCache/Async/TestReactor.cpp"
        Dispatch_test.cpp
        EndpointDial_test.cpp
        DependencyOutput_test.cpp
        DependencyProbe_test.cpp
        DirectManifest_test.cpp
        PathResolve_test.cpp
        ReplayGuard_test.cpp
        RootReconciler_test.cpp
        ProcessRunner_test.cpp
        Stats_test.cpp
        ToolchainDiscovery_test.cpp
        ToolchainFingerprint_test.cpp
        ToolchainHost_test.cpp
        ToolchainProbe_test.cpp
        WorkerProtocol_test.cpp
        ${_fc_cc_core}
    )
    target_include_directories(fastcache-cc-tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
    if(WIN32)
        # Same reason as the launcher's: this target compiles the same
        # ${_fc_cc_core} rows and links FastCache no more than the launcher does.
        target_compile_definitions(fastcache-cc-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    endif()
    target_link_libraries(fastcache-cc-tests PRIVATE Catch2::Catch2 Threads::Threads)
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
