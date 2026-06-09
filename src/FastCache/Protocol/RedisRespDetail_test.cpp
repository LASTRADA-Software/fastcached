// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Protocol/RedisRespDetail.hpp>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

TEST_CASE("SwallowDestructorException returns normally on a non-throwing callable", "[protocol][resp][cleanup-noexcept]")
{
    bool ran = false;
    FastCache::Detail::SwallowDestructorException([&] { ran = true; });
    REQUIRE(ran);
}

TEST_CASE("SwallowDestructorException swallows std::runtime_error from the callable", "[protocol][resp][cleanup-noexcept]")
{
    // The destructor-noexcept contract: ~Cleanup's teardown steps must
    // never propagate. Without this swallow, the implicitly-noexcept
    // destructor would std::terminate the process — leaking the watcher
    // coroutine frame and any stale WATCH index entries that the
    // subsequent steps would have cleaned up. We verify the swallow
    // directly via the helper that ~Cleanup invokes.
    FastCache::Detail::SwallowDestructorException([] { throw std::runtime_error { "synthetic lock failure" }; });
    SUCCEED("control returned after the throwing callable");
}

TEST_CASE("SwallowDestructorException swallows non-std exceptions too", "[protocol][resp][cleanup-noexcept]")
{
    // A std::scoped_lock that runs out of kernel mutex resources throws
    // std::system_error; but the catch-all also covers user-thrown
    // non-std payloads — anything the runtime can carry through a
    // catch(...) is swallowed.
    FastCache::Detail::SwallowDestructorException([] { throw 42; });
    SUCCEED("control returned after the throwing int");
}

TEST_CASE("SwallowDestructorException, when chained, runs every later step even after an earlier throw",
          "[protocol][resp][cleanup-noexcept]")
{
    // This is the actual ~Cleanup pattern: three calls in sequence; if
    // the first throws, the second and third must still run. Drive the
    // same shape and verify the side-effects.
    int stepsRun = 0;
    FastCache::Detail::SwallowDestructorException([&] {
        ++stepsRun;
        throw std::runtime_error { "step 1 threw" };
    });
    FastCache::Detail::SwallowDestructorException([&] { ++stepsRun; });
    FastCache::Detail::SwallowDestructorException([&] { ++stepsRun; });
    REQUIRE(stepsRun == 3);
}
