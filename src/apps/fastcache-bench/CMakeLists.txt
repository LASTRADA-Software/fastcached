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

add_executable(fastcache-bench StorageBench.cpp)
target_link_libraries(fastcache-bench PRIVATE FastCache Catch2::Catch2WithMain)
