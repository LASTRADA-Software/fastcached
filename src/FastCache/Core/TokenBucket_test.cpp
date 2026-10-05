// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/TokenBucket.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>

using namespace std::chrono_literals;
using FastCache::TokenBucket;

namespace
{

/// Take from @p bucket until it refuses, at @p now.
/// @param bucket The bucket.
/// @param now The time of every take.
/// @return How many were taken.
[[nodiscard]] std::size_t Drain(TokenBucket& bucket, core::platform::SteadyTimePoint now)
{
    auto taken = std::size_t { 0 };
    while (bucket.TryTake(now))
        ++taken;
    return taken;
}

} // namespace

TEST_CASE("A token bucket starts full, grants its burst, and then refuses", "[core][tokenbucket]")
{
    auto const start = core::platform::SteadyTimePoint {};
    TokenBucket bucket { 8, 100ms, start };
    CHECK(Drain(bucket, start) == 8);
    CHECK_FALSE(bucket.TryTake(start));
}

TEST_CASE("A token bucket earns one token per interval, never more than it holds", "[core][tokenbucket]")
{
    auto const start = core::platform::SteadyTimePoint {};
    TokenBucket bucket { 8, 100ms, start };
    REQUIRE(Drain(bucket, start) == 8);

    CHECK_FALSE(bucket.TryTake(start + 99ms));
    CHECK(bucket.TryTake(start + 100ms));
    CHECK_FALSE(bucket.TryTake(start + 100ms));
    CHECK(Drain(bucket, start + 400ms) == 3);

    // A long silence refills it to its capacity and no further.
    CHECK(Drain(bucket, start + 1h) == 8);
}

TEST_CASE("A token bucket keeps the part of an interval already elapsed", "[core][tokenbucket]")
{
    // A token earned at 150 ms leaves half an interval over, and that half counts toward the next
    // token rather than being rounded away: the next is due at 200 ms, not at 250 ms.
    auto const start = core::platform::SteadyTimePoint {};
    TokenBucket bucket { 4, 100ms, start };
    REQUIRE(Drain(bucket, start) == 4);
    CHECK(bucket.TryTake(start + 150ms));
    CHECK(bucket.TryTake(start + 200ms));
    CHECK_FALSE(bucket.TryTake(start + 250ms));
    CHECK(bucket.TryTake(start + 300ms));
}

TEST_CASE("A clock that stands still or steps back grants a token bucket nothing", "[core][tokenbucket]")
{
    auto const start = core::platform::SteadyTimePoint {} + 1h;
    TokenBucket bucket { 2, 100ms, start };
    REQUIRE(Drain(bucket, start) == 2);
    CHECK_FALSE(bucket.TryTake(start - 30min));
    CHECK_FALSE(bucket.TryTake(start));
    CHECK(bucket.TryTake(start + 100ms));
}
