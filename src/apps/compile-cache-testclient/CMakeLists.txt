# SPDX-License-Identifier: Apache-2.0
# Standalone compile-cache test client. Links the FastCache library so it
# reuses the exact PathCanon / CompileValue codec the server uses — the
# canonicalization parity contract is exercised, not reimplemented.

add_executable(compile-cache-testclient main.cpp TestClientCli.cpp)
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
