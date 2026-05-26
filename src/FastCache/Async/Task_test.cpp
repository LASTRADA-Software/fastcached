// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace
{

FastCache::Task<int> ReturnFortyTwo()
{
    co_return 42;
}

FastCache::Task<int> CallReturnFortyTwo()
{
    auto const value = co_await ReturnFortyTwo();
    co_return value + 1;
}

FastCache::Task<std::string> Greet(std::string name)
{
    co_return "hello, " + name;
}

FastCache::Task<void> JustReturn(int* sideEffect)
{
    *sideEffect = 7;
    co_return;
}

FastCache::Task<int> Throws()
{
    throw std::runtime_error { "boom" };
    co_return 0; // unreachable, here so the function is a coroutine
}

FastCache::Task<int> CallsThrows()
{
    auto const v = co_await Throws();
    co_return v;
}

} // namespace

TEST_CASE("Task<int> runs to completion and yields the awaited value", "[task]")
{
    auto const result = FastCache::SyncRun(ReturnFortyTwo());
    REQUIRE(result == 42);
}

TEST_CASE("Task<int> chains via co_await without exception", "[task]")
{
    auto const result = FastCache::SyncRun(CallReturnFortyTwo());
    REQUIRE(result == 43);
}

TEST_CASE("Task<string> moves its result out", "[task]")
{
    auto const result = FastCache::SyncRun(Greet("world"));
    REQUIRE(result == "hello, world");
}

TEST_CASE("Task<void> runs the body for side effects", "[task]")
{
    int sideEffect = 0;
    FastCache::SyncRun(JustReturn(&sideEffect));
    REQUIRE(sideEffect == 7);
}

TEST_CASE("Task propagates exceptions through co_await", "[task]")
{
    REQUIRE_THROWS_AS(FastCache::SyncRun(CallsThrows()), std::runtime_error);
}
