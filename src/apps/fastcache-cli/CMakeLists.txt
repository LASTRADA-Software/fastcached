# SPDX-License-Identifier: Apache-2.0
# fastcache-cli — the operator's client for the cache and the compile fleet.
#
# Links the whole FastCache library, the way fastcached and compile-cache-testclient
# do, because it needs the library's own types rather than a copy of them: Cluster's
# state codec, FleetView's shared column table, and MetricsCatalog. That is the
# opposite choice from fastcache-cc, which deliberately links nothing so it can be
# spawned per translation unit with a static CRT — and it is the right one here,
# because this binary runs once when a person types a command.
#
# Linking FastCache also brings the WIN32_LEAN_AND_MEAN / NOMINMAX / _WIN32_WINNT trio
# transitively (they are PUBLIC on that target), which is why — unlike the launcher —
# nothing here re-declares them.

# Everything except main.cpp, so the test binary can compile the same set.
#
# main.cpp is deliberately NOT in this list: it is in no test target, which is a
# property this tree has paid for twice (#370, #909), and it is why every decision
# this tool makes lives in one of the files below instead.
set(_fc_cli_sources
    CliAnswer.cpp
    CliCommand.cpp
    CliFormat.cpp
    CliValue.cpp
    CliVerbs.cpp
    MemcachedClient.cpp
    RespClient.cpp
    SocketExchange.cpp
    StatsGatherer.cpp
    StatsSource.cpp
)

add_executable(fastcache-cli main.cpp ${_fc_cli_sources})
target_include_directories(fastcache-cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fastcache-cli PRIVATE FastCache)

# The version is compiled in from the git tag rather than read at runtime, so
# `fastcache-cli version` cannot report a value that has drifted from the build. The
# source refuses to compile without it.
target_compile_definitions(fastcache-cli PRIVATE FASTCACHE_CLI_VERSION="${FASTCACHED_VERSION_STRING}")

install(TARGETS fastcache-cli
    RUNTIME DESTINATION "${FASTCACHED_INSTALL_BINDIR}"
    COMPONENT Runtime
)

# The command line, the verb handlers, the formatters and the stats ladder's choice are
# all pure functions over injected seams, so they are unit-testable with no daemon and
# no socket. Its own binary rather than rows in FastCacheTest: the library's test glob
# only reaches src/FastCache.
if(FASTCACHED_BUILD_TESTS)
    add_executable(fastcache-cli-tests
        ${_fc_cli_sources}
        CliCommand_test.cpp
        CliFormat_test.cpp
        CliValue_test.cpp
        CliVerbs_test.cpp
        MemcachedClient_test.cpp
        MemcachedVerbs_test.cpp
        RespClient_test.cpp
        StatsSource_test.cpp
    )
    target_include_directories(fastcache-cli-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(fastcache-cli-tests PRIVATE FastCache Catch2::Catch2WithMain)
    target_compile_definitions(fastcache-cli-tests PRIVATE FASTCACHE_CLI_VERSION="${FASTCACHED_VERSION_STRING}")
    include(Catch)
    # `SKIP_RETURN_CODE 4` tells ctest a Catch2 skip is a skip and not a failure
    # (#499) -- and it is KNOWN BROKEN in the other direction: Catch2's exit code IS
    # its failed-assertion count, so a case failing exactly four assertions is scored
    # *skipped* (#1128). It stays because all three alternative channels were measured
    # and are worse; the argument is at the top of the top-level CMakeLists and the
    # fix is #1152. `catch-skip-return-code` enforces the property, and its green does
    # not mean the property is sound.
    catch_discover_tests(fastcache-cli-tests PROPERTIES SKIP_RETURN_CODE 4)
endif()
