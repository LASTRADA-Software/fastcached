// SPDX-License-Identifier: Apache-2.0
#include "TerminalEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <string>
#include <thread>

#include <tests/ScopedPipeStdin.hpp>

using namespace FastCache;
using namespace FastCache::Cli;
using FastCache::Testing::ScopedPipeStdin;

namespace
{

/// Drain @p reactor until @p done, failing by name if it never happens.
///
/// Bounded on the MONOTONIC clock, so a start that never resumes is a red naming what it waited
/// for rather than a ctest timeout naming nothing.
/// @param reactor The reactor resumptions come back through.
/// @param done The condition waited for.
/// @param what What it means if it never becomes true.
template <typename Condition>
void DrainUntil(TestReactor& reactor, Condition done, char const* what)
{
    auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!done())
    {
        if (std::chrono::steady_clock::now() > deadline)
            FAIL(what);
        reactor.Drain();
        std::this_thread::yield();
    }
}

/// Start @p terminal, writing the result where the caller can read it.
[[nodiscard]] Task<void> StartInto(std::unique_ptr<UnstartedTerminal> terminal,
                                   std::expected<StartedTerminal, std::string>* out,
                                   bool* delivered)
{
    *out = co_await StartTerminal(std::move(terminal));
    *delivered = true;
}

} // namespace

TEST_CASE("a terminal is made without one, and starting it with none refuses by saying so", "[cli][dashboard][terminal]")
{
    // Making it with stdin a pipe must succeed, because making one asks nothing: a composition may
    // make it and never start it. Only the start asks whether there is a terminal.
    ScopedPipeStdin const guard;
    REQUIRE(guard.installed);
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto pool = ThreadPoolExecutor { 1 };

    auto made = MakeTerminalEvents(&pool, &reactor, UsageColor::Plain);
    REQUIRE(made.has_value());

    auto started = std::expected<StartedTerminal, std::string> { std::unexpected(std::string { "never delivered" }) };
    auto delivered = false;
    auto task = StartInto(std::move(*made), &started, &delivered);
    reactor.Submit(task.Native());
    DrainUntil(reactor, [&delivered] { return delivered; }, "StartTerminal never resumed");

    REQUIRE_FALSE(started.has_value());
    CHECK(started.error() == "standard input and output are not both an interactive terminal");
}
