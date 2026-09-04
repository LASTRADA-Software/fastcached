// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Server/ReadinessAnnouncer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace FastCache;

namespace
{

/// The literal every waiter outside this repository greps for.
///
/// `bench/runner.py` (`READY_MARKER`) and the packaged-service step in
/// `.github/workflows/build.yml` both match this substring, and neither can be
/// recompiled from here. #646 made the line mean something STRONGER without
/// rewording it, precisely so those two keep working -- so the wording is pinned by
/// its bytes, the way a wire constant is, rather than by a symbol both ends spell.
constexpr std::string_view ReadyMarker = "ready, accepting connections";

/// Indices of the records whose message contains `needle`.
/// @param records Captured log records, in order.
/// @param needle Substring to look for.
/// @return Positions, ascending.
[[nodiscard]] std::vector<std::size_t> IndicesContaining(std::vector<CapturingLogger::Record> const& records,
                                                         std::string_view needle)
{
    std::vector<std::size_t> found;
    for (auto const index: std::views::iota(std::size_t { 0 }, records.size()))
        if (records[index].message.contains(needle))
            found.push_back(index);
    return found;
}

} // namespace

TEST_CASE("ReadinessAnnouncer says nothing until the last acceptor arms", "[server][readiness]")
{
    // The property #646 is about, at the primitive: readiness is announced AFTER the
    // acceptors, never before. A three-acceptor announcer that spoke on the first arm
    // would be the same defect the ticket describes with a counter bolted on.
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, 3, "3 bind(s)" };

    CHECK_FALSE(announcer.Announced());
    CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).empty());

    announcer.AcceptorArmed("bind 0");
    CHECK_FALSE(announcer.Announced());
    CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).empty());

    announcer.AcceptorArmed("bind 1");
    CHECK_FALSE(announcer.Announced());
    CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).empty());

    announcer.AcceptorArmed("bind 2");
    CHECK(announcer.Announced());

    auto const records = logger.Snapshot();
    auto const ready = IndicesContaining(records, ReadyMarker);
    REQUIRE(ready.size() == 1);
    CHECK(records[ready.front()].level == LogLevel::Info);
    CHECK(records[ready.front()].message.contains("3 bind(s)"));

    // And it is the LAST record: every arm is reported before it, which is the
    // ordering an operator reads and a fixture depends on.
    CHECK(ready.front() == records.size() - 1);
    CHECK(IndicesContaining(records, "acceptor armed").size() == 3);
}

TEST_CASE("ReadinessAnnouncer announces exactly once however many threads arm", "[server][readiness]")
{
    // Production arms from several threads on both multi-reactor paths: on POSIX each
    // reactor arms its own accept loops on its own thread, on Windows each bind has an
    // acceptor thread. A second readiness line would tell a waiter a second daemon had
    // come up, so the transition is observed by exactly one caller.
    constexpr std::size_t Acceptors = 16;
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, Acceptors, "16 bind(s) x 1 reactors" };

    std::vector<std::jthread> arms;
    arms.reserve(Acceptors);
    for (auto const index: std::views::iota(std::size_t { 0 }, Acceptors))
        arms.emplace_back([&announcer, index] { announcer.AcceptorArmed(std::to_string(index)); });
    arms.clear();

    CHECK(announcer.Announced());
    CHECK(announcer.ArmedCount() == Acceptors);

    auto const records = logger.Snapshot();
    CHECK(IndicesContaining(records, ReadyMarker).size() == 1);
    CHECK(IndicesContaining(records, "acceptor armed").size() == Acceptors);
}

TEST_CASE("ReadinessAnnouncer reports an overshoot rather than announcing twice", "[server][readiness]")
{
    // A caller that declared fewer acceptors than it went on to arm has mis-wired the
    // count, and the readiness line it produced named fewer endpoints than are being
    // served. Silently re-announcing would hide that from the operator AND hand a
    // waiter a second marker; the overshoot is a Warn and the marker stays unique.
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, 1, "1 bind(s)" };

    announcer.AcceptorArmed("bind 0");
    announcer.AcceptorArmed("bind 1");

    auto const records = logger.Snapshot();
    CHECK(IndicesContaining(records, ReadyMarker).size() == 1);

    auto const warnings =
        std::ranges::count_if(records, [](CapturingLogger::Record const& record) { return record.level == LogLevel::Warn; });
    CHECK(warnings == 1);
    CHECK(IndicesContaining(records, "under-reports").size() == 1);
    CHECK(announcer.ArmedCount() == 2);
}

TEST_CASE("ReadinessAnnouncer with no acceptors announces nothing", "[server][readiness]")
{
    // Absent is not zero, and it is not "ready" either. A server with no acceptor is
    // not one that is ready with none -- `RunReactorServer` refuses an empty bind list
    // a step earlier, and this is the announcer being total about the same fact rather
    // than relying on that refusal staying where it is.
    CapturingLogger logger;
    ReadinessAnnouncer const announcer { logger, 0, "0 bind(s)" };

    CHECK_FALSE(announcer.Announced());
    CHECK(announcer.AcceptorCount() == 0);
    CHECK(logger.Snapshot().empty());
}
