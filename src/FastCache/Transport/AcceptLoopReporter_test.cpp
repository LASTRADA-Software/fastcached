// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Transport/AcceptLoopReporter.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <tuple>

#include <core/net/AcceptLoopHealth.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/NetError.hpp>
#include <core/platform/Clock.hpp>

namespace
{

using core::net::AcceptErrorPolicy;
using core::net::AcceptLoopEventKind;
using core::net::NetErrorCode;
using FastCache::AcceptLoopNext;
using FastCache::AcceptLoopReporter;
using FastCache::LogLevel;

/// @param code What the accept answered.
/// @return A failed accept as a loop meets it.
[[nodiscard]] core::net::NetError Failed(NetErrorCode code)
{
    return core::net::makeNetError(code, 0, "accept");
}

/// @param logger The lines a case captured.
/// @param level The level asked about.
/// @return How many lines went out at @p level.
[[nodiscard]] std::size_t LinesAt(FastCache::CapturingLogger const& logger, LogLevel level)
{
    return static_cast<std::size_t>(std::ranges::count_if(
        logger.Snapshot(), [level](FastCache::CapturingLogger::Record const& record) { return record.level == level; }));
}

/// One loop's reporter over a registry and a log, with the loop's clock.
struct Loop
{
    FastCache::CapturingLogger logger;
    core::net::AcceptLoopHealth health;
    AcceptLoopReporter reporter { "raft: peer", "raft", logger, health };
    core::platform::SteadyTimePoint now {};
};

} // namespace

TEST_CASE("A failed connection is accepted past with one warning, and changes no condition", "[net][accept-loop]")
{
    Loop loop;
    auto const step = loop.reporter.OnError(Failed(NetErrorCode::ConnReset), loop.now, false);
    CHECK(step.next == AcceptLoopNext::AcceptAgain);
    CHECK(step.delay == std::chrono::milliseconds {});
    CHECK(LinesAt(loop.logger, LogLevel::Warn) == 1);
    CHECK(loop.logger.Snapshot().front().message.starts_with("raft: peer:"));
    CHECK(loop.health.snapshot().empty());
}

TEST_CASE("A long run of unclassified failures is reported degraded once, and an accept clears it", "[net][accept-loop]")
{
    Loop loop;
    auto degradedAt = std::size_t { 0 };
    for (auto const failure: std::views::iota(std::uint32_t { 1 }, AcceptErrorPolicy::UnclassifiedBeforeDegraded + 1))
    {
        auto const step = loop.reporter.OnError(Failed(NetErrorCode::SystemError), loop.now, false);
        // Backed off on, never ended: what nobody classified is no reason to stop serving.
        REQUIRE(step.next == AcceptLoopNext::AcceptAgain);
        CHECK(step.delay > std::chrono::milliseconds {});
        loop.now += step.delay;
        if (degradedAt == 0 && !loop.health.snapshot().empty())
            degradedAt = failure;
    }
    CHECK(degradedAt == AcceptErrorPolicy::UnclassifiedBeforeDegraded);
    auto const degraded = loop.health.snapshot();
    REQUIRE(degraded.size() == 1);
    // Under the registry's name, which `/healthz` and the conditions print.
    CHECK(degraded.front().surface == "raft");
    CHECK(degraded.front().kind == AcceptLoopEventKind::Degraded);
    CHECK(LinesAt(loop.logger, LogLevel::Error) == 1);

    loop.reporter.OnAccepted(loop.now);
    CHECK(loop.health.snapshot().empty());
    CHECK(LinesAt(loop.logger, LogLevel::Info) == 1);
}

TEST_CASE("A degraded loop its owner stops says so, and leaves nothing in the registry", "[net][accept-loop]")
{
    Loop loop;
    for ([[maybe_unused]] auto const failure:
         std::views::iota(std::uint32_t { 0 }, AcceptErrorPolicy::UnclassifiedBeforeDegraded))
        std::ignore = loop.reporter.OnError(Failed(NetErrorCode::SystemError), loop.now, false);
    REQUIRE(loop.health.snapshot().size() == 1);

    auto const step = loop.reporter.OnError(Failed(NetErrorCode::Cancelled), loop.now, true);
    CHECK(step.next == AcceptLoopNext::End);
    CHECK(loop.health.snapshot().empty());

    // The control: the same owner-stopped close of a loop that was never degraded says nothing.
    Loop healthy;
    CHECK(healthy.reporter.OnError(Failed(NetErrorCode::Cancelled), healthy.now, true).next == AcceptLoopNext::End);
    CHECK(healthy.logger.Snapshot().empty());
    CHECK(healthy.health.snapshot().empty());
}

TEST_CASE("A listener closed under a loop that was serving, or found dead, is a surface that stopped", "[net][accept-loop]")
{
    SECTION("closed while nobody was stopping: the loop ends, and the registry says it gave up")
    {
        Loop loop;
        auto const step = loop.reporter.OnError(Failed(NetErrorCode::Cancelled), loop.now, false);
        CHECK(step.next == AcceptLoopNext::End);
        auto const stopped = loop.health.snapshot();
        REQUIRE(stopped.size() == 1);
        CHECK(stopped.front().kind == AcceptLoopEventKind::GaveUp);
        CHECK(LinesAt(loop.logger, LogLevel::Error) == 1);
    }

    SECTION("a dead handle while nobody was stopping: the loop closes the listener, and gave up")
    {
        Loop loop;
        auto const step = loop.reporter.OnError(Failed(NetErrorCode::BadHandle), loop.now, false);
        CHECK(step.next == AcceptLoopNext::EndAndClose);
        auto const stopped = loop.health.snapshot();
        REQUIRE(stopped.size() == 1);
        CHECK(stopped.front().kind == AcceptLoopEventKind::GaveUp);
        CHECK(stopped.front().reason.contains("bad handle"));
        CHECK(LinesAt(loop.logger, LogLevel::Error) == 1);
    }
}

TEST_CASE("A dead handle met while the owner stops is the stop, not a surface that gave up", "[net][accept-loop]")
{
    // On Windows the teardown closes a listener an acceptor thread may be about to accept on, and
    // that accept answers WSAENOTSOCK, a dead handle. Reported, an orderly stop would write an Error
    // line and latch `surface-not-accepting`, which no later event clears.
    Loop loop;
    auto const step = loop.reporter.OnError(Failed(NetErrorCode::BadHandle), loop.now, true);
    CHECK(step.next == AcceptLoopNext::End);
    CHECK(LinesAt(loop.logger, LogLevel::Error) == 0);
    CHECK(loop.health.snapshot().empty());

    // And a loop that WAS degraded when the stop met it leaves nothing degraded behind, as a
    // cancelled one does.
    Loop degraded;
    for ([[maybe_unused]] auto const failure:
         std::views::iota(std::uint32_t { 0 }, AcceptErrorPolicy::UnclassifiedBeforeDegraded))
        std::ignore = degraded.reporter.OnError(Failed(NetErrorCode::SystemError), degraded.now, false);
    REQUIRE(degraded.health.snapshot().size() == 1);
    CHECK(degraded.reporter.OnError(Failed(NetErrorCode::BadHandle), degraded.now, true).next == AcceptLoopNext::End);
    CHECK(degraded.health.snapshot().empty());
}

TEST_CASE("Exhaustion is backed off on, doubling, and an accept starts the backoff again", "[net][accept-loop]")
{
    Loop loop;
    auto const first = loop.reporter.OnError(Failed(NetErrorCode::ResourceExhausted), loop.now, false);
    auto const second = loop.reporter.OnError(Failed(NetErrorCode::ResourceExhausted), loop.now, false);
    CHECK(first.delay == AcceptErrorPolicy::FirstBackoff);
    CHECK(second.delay == 2 * AcceptErrorPolicy::FirstBackoff);
    loop.reporter.OnAccepted(loop.now);
    CHECK(loop.reporter.OnError(Failed(NetErrorCode::ResourceExhausted), loop.now, false).delay
          == AcceptErrorPolicy::FirstBackoff);
    CHECK(loop.health.snapshot().empty());
}
