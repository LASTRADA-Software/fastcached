// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Server/ReadinessAnnouncer.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
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

/// Register `count` participants and close the set, the way a spawn loop does.
/// @param announcer The announcer to prime.
/// @param count How many participants will arm.
void ExpectAndSeal(ReadinessAnnouncer& announcer, std::size_t count)
{
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, count))
        announcer.ExpectAcceptor();
    announcer.AcceptorsAllSpawned();
}

} // namespace

TEST_CASE("ReadinessAnnouncer says nothing until the last acceptor arms", "[server][readiness]")
{
    // The property #646 is about, at the primitive: readiness is announced AFTER the
    // acceptors, never before. A three-acceptor announcer that spoke on the first arm
    // would be the same defect the ticket describes with a counter bolted on.
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, "3 bind(s)" };
    ExpectAndSeal(announcer, 3);

    CHECK(announcer.ExpectedCount() == 3);
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

TEST_CASE("ReadinessAnnouncer cannot announce before the set of acceptors is closed", "[server][readiness]")
{
    // A participant that arms while its siblings are still being created must not
    // announce on their behalf. On the multi-reactor paths the spawn loop and the
    // arming genuinely overlap -- an acceptor thread can be parked in `accept()`
    // before the loop has finished emplacing the rest -- so "every acceptor
    // registered SO FAR has armed" is a state that really occurs and is not
    // readiness.
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, "2 bind(s)" };

    announcer.ExpectAcceptor();
    announcer.AcceptorArmed("bind 0");
    // One registered, one armed. Nothing may be claimed: the loop has not said it is
    // done, so there may be more to come -- and there are.
    CHECK_FALSE(announcer.Announced());
    CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).empty());

    announcer.ExpectAcceptor();
    announcer.AcceptorArmed("bind 1");
    CHECK_FALSE(announcer.Announced());

    // Closing the set is what completes the condition here, so the seal is the
    // announcement. Whichever of the two arrives last has to be able to announce, or
    // a run where every acceptor armed early would never report readiness at all.
    announcer.AcceptorsAllSpawned();
    CHECK(announcer.Announced());
    CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).size() == 1);
}

TEST_CASE("ReadinessAnnouncer announces exactly once however many threads arm", "[server][readiness]")
{
    // Production arms from several threads on both multi-reactor paths: on POSIX each
    // reactor arms its own accept loops on its own thread, on Windows each bind has an
    // acceptor thread. A second readiness line would tell a waiter a second daemon had
    // come up, so the transition is observed by exactly one caller.
    constexpr std::size_t Acceptors = 16;
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, "16 bind(s) x 1 reactors" };
    ExpectAndSeal(announcer, Acceptors);

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

TEST_CASE("ReadinessAnnouncer reports an acceptor registered after the set was closed", "[server][readiness]")
{
    // A participant created after the seal can never be counted, so the readiness
    // line describes fewer endpoints than are being served. That is a wiring mistake
    // in the spawn loop and it is invisible from the readiness line itself, so it is
    // reported rather than absorbed.
    CapturingLogger logger;
    ReadinessAnnouncer announcer { logger, "1 bind(s)" };
    ExpectAndSeal(announcer, 1);
    announcer.AcceptorArmed("bind 0");
    REQUIRE(announcer.Announced());

    announcer.ExpectAcceptor();

    // The late registration is refused rather than silently changing the total, so
    // the line that was already emitted does not retroactively become wrong.
    CHECK(announcer.ExpectedCount() == 1);
    auto const records = logger.Snapshot();
    CHECK(IndicesContaining(records, "registered after the set was closed").size() == 1);
    CHECK(IndicesContaining(records, ReadyMarker).size() == 1);
}

TEST_CASE("ReadinessAnnouncer says so when it ends without ever announcing", "[server][readiness]")
{
    // The one state that would otherwise have no line of its own, and the reason the
    // count is registered rather than computed: a total that came out too HIGH stops
    // the readiness line ever being emitted, and two out-of-tree waiters plus a CI
    // step block on that line. A contract that can silently never fire is worse than
    // one that fires early, because early is at least observable.
    //
    // So the failing case is made to explain itself, naming how many of how many
    // armed -- which is what separates a daemon that failed to bind from one whose
    // acceptors were still coming up when it was told to stop.
    CapturingLogger logger;
    {
        ReadinessAnnouncer announcer { logger, "4 bind(s)" };
        ExpectAndSeal(announcer, 4);
        announcer.AcceptorArmed("bind 0");
        REQUIRE_FALSE(announcer.Announced());
    }

    auto const records = logger.Snapshot();
    CHECK(IndicesContaining(records, ReadyMarker).empty());

    auto const never = IndicesContaining(records, "readiness was never announced");
    REQUIRE(never.size() == 1);
    CHECK(records[never.front()].level == LogLevel::Warn);
    CHECK(records[never.front()].message.contains("1 of 4"));
}

TEST_CASE("ReadinessAnnouncer stays silent about a run that did announce", "[server][readiness]")
{
    // The mirror of the case above, and it is not redundant: a destructor that warned
    // unconditionally would put a "never announced" line into every clean shutdown,
    // which is the same defect as #603 -- a signal an operator learns to ignore.
    CapturingLogger logger;
    {
        ReadinessAnnouncer announcer { logger, "1 bind(s)" };
        ExpectAndSeal(announcer, 1);
        announcer.AcceptorArmed("bind 0");
        REQUIRE(announcer.Announced());
    }

    CHECK(IndicesContaining(logger.Snapshot(), "readiness was never announced").empty());
}

TEST_CASE("ReadinessAnnouncer with no acceptors announces nothing", "[server][readiness]")
{
    // Absent is not zero, and it is not "ready" either. A server with no acceptor is
    // not one that is ready with none -- `RunReactorServer` refuses an empty bind list
    // a step earlier, and this is the announcer being total about the same fact rather
    // than relying on that refusal staying where it is.
    CapturingLogger logger;
    {
        ReadinessAnnouncer announcer { logger, "0 bind(s)" };
        announcer.AcceptorsAllSpawned();

        CHECK_FALSE(announcer.Announced());
        CHECK(announcer.ExpectedCount() == 0);
        CHECK(IndicesContaining(logger.Snapshot(), ReadyMarker).empty());
    }

    // And it explains itself on the way out rather than leaving an absence.
    CHECK(IndicesContaining(logger.Snapshot(), "readiness was never announced").size() == 1);
}
