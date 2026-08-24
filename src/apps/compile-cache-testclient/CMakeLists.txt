# SPDX-License-Identifier: Apache-2.0
# Standalone compile-cache test client. Links the FastCache library so it
# reuses the exact PathCanon / CompileValue codec the server uses — the
# canonicalization parity contract is exercised, not reimplemented.

# ProcessRunner.cpp is compiled in from the launcher rather than copied, exactly
# as fastcache-compile-node does with it: spawning a compiler is ambient I/O with
# a wildly different implementation per platform, and a fourth hand-written
# version here is what issue #84 was about.
add_executable(compile-cache-testclient
    main.cpp
    TestClientCli.cpp
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ProcessRunner.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/ReplayGuard.cpp"
    "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc/DirectManifest.cpp")
target_include_directories(compile-cache-testclient PRIVATE "${CMAKE_SOURCE_DIR}/src/apps/fastcache-cc")
target_link_libraries(compile-cache-testclient PRIVATE FastCache)

# The CLI is a pure function over argv, so it is unit-testable without a daemon
# or a compiler. Its own binary rather than a row in FastCacheTest: the test
# glob only reaches src/FastCache, and this tool is gated off by default.
if(FASTCACHED_BUILD_TESTS)
    add_executable(compile-cache-testclient-tests
        TestClientCli.cpp
        TestClientCli_test.cpp
    )
    target_include_directories(compile-cache-testclient-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(compile-cache-testclient-tests PRIVATE FastCache Catch2::Catch2WithMain)
    include(Catch)
    catch_discover_tests(compile-cache-testclient-tests)
endif()
