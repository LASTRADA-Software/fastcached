# SPDX-License-Identifier: Apache-2.0
# In-process micro-benchmarks for the storage stack. Test infrastructure, never
# installed — the same footing as compile-cache-testclient.
#
# Uses Catch2's benchmark support rather than adding a benchmark library: Catch2
# is already fetched for the test suite, so this target costs no new dependency.
# That also makes FASTCACHED_BUILD_TESTS a hard prerequisite, since it is what
# brings Catch2 into the build.

if(NOT FASTCACHED_BUILD_TESTS)
    message(FATAL_ERROR
        "FASTCACHED_BUILD_BENCHMARKS=ON requires FASTCACHED_BUILD_TESTS=ON "
        "(the benchmarks use Catch2, which the test suite pulls in).")
endif()

add_executable(fastcache-bench
    BuildBanner.cpp
    BuildBannerListener.cpp
    BuildBanner_test.cpp
    LiveStatsBench.cpp
    RaftPeerFrameBench.cpp
    Sha256Bench.cpp
    StorageBench.cpp
    StreamCodecBench.cpp
)
target_link_libraries(fastcache-bench PRIVATE FastCache Catch2::Catch2WithMain)

# What the build system was ASKED for, so the banner can print it as a LABEL.
#
# `$<CONFIG>` rather than `${CMAKE_BUILD_TYPE}`: a multi-config generator leaves that variable
# empty and resolves the configuration at build time, so the variable would put "" into a
# binary that is perfectly well a Release one. Empty is still a legal answer here -- a
# single-config tree configured without a build type -- and `BuildBanner.cpp` renders it as
# "(not set)" rather than as a configuration nobody chose.
#
# No verdict in the banner reads this. It is the one fact there that comes from CMake rather
# than from the compiler, and `.agent/rules/build-and-toolchain.md` carries why: the cache and
# the configure log are precisely the two places that have been confidently wrong about what
# the compile line got. `BuildBanner.hpp` states the rule; `BuildBanner_test.cpp` pins it with
# two readings that differ only in this label and must reach the same verdict.
target_compile_definitions(fastcache-bench PRIVATE FASTCACHE_BENCH_BUILD_TYPE="$<CONFIG>")

# No `catch_discover_tests` here, deliberately. `scripts/check-tsan-binaries.cmake` derives
# "every Catch2 test binary this tree registers" from exactly that call, and requires each to
# be a row of the ThreadSanitizer gate's TARGETS table or to carry an exemption whose claim is
# re-scanned -- and this directory names `std::thread` (StorageBench's scaling tier), so no
# exemption here could be honest. Registering the banner's cases that way would therefore mean
# putting a benchmark binary, scaling tier and all, under TSan. `src/tests/CMakeLists.txt`
# registers `bench-build-banner` instead: one plain `add_test` running `[buildbanner]`, which
# selects the only non-hidden non-benchmark cases in this binary.
