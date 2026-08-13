// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>

#include <catch2/catch_test_macros.hpp>

#include <coroutine>
#include <stdexcept>
#include <string>

namespace
{

/// Awaitable that suspends and is never resumed by anybody — the shape of a
/// socket read with no data buffered and no closed peer to report EOF.
struct NeverCompletes
{
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() const noexcept {}
};

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

/// Value-returning task that parks before ever producing a result.
FastCache::Task<int> ParksForever()
{
    co_await NeverCompletes {};
    co_return 1;
}

/// void task that parks before its side effect, so a test can assert the body
/// past the suspend point did not run.
FastCache::Task<void> ParksForeverVoid(int* sideEffect)
{
    co_await NeverCompletes {};
    *sideEffect = 1;
    co_return;
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

TEST_CASE("SyncRun refuses a task that is still suspended (regression)", "[task][regression]")
{
    // SyncRun used to read promise.result unconditionally. For a task still
    // parked after resume() that names a variant alternative which was never
    // engaged, and ~Task() then tears the frame down while whatever parked the
    // coroutine still points into it — the drain loops in the protocol tests
    // park exactly this way when a reply is an exact multiple of their chunk
    // size, and it surfaced as a SIGSEGV / heap corruption / abort rather than
    // as a named failure. It must be a diagnosable precondition violation.
    REQUIRE_THROWS_AS(FastCache::SyncRun(ParksForever()), std::logic_error);
    int sideEffect = 0;
    REQUIRE_THROWS_AS(FastCache::SyncRun(ParksForeverVoid(&sideEffect)), std::logic_error);
    REQUIRE(sideEffect == 0);
}
