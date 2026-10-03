// SPDX-License-Identifier: Apache-2.0
//
// How loudly a scheduler that does not answer, or refuses, is said. Both announce loops used to log
// every unreachable round and every refusal at Warn -- one line per loop per round, about 360 an
// hour on a machine running both -- which is a log nobody reads for the one line that changed.
// These cases drive the tables with a `ManualClock` and count lines by level, which is the whole
// property.
#include "NodeConditions.hpp"
#include "SchedulerReachability.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

#include <core/Ranges.hpp>
#include <core/platform/Clock.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace
{

constexpr std::string_view SchedulerA = "scheduler-a.example:6676";
constexpr std::string_view SchedulerB = "scheduler-b.example:6676";

/// One dial per round, at the announce loops' cadence. Not `NodeAnnounceInterval`: this file tests
/// the tables, and pulling in the announce header would make it about the loop.
constexpr auto Round = std::chrono::seconds { 20 };

/// One hour of rounds.
constexpr int Rounds = 180;

/// How many lines of each level a run produced.
using Tally = std::map<LogLevel, std::size_t>;

/// A dial of @p endpoint that did not connect, going nowhere next.
/// @param endpoint The scheduler.
/// @return The failure.
[[nodiscard]] SchedulerFailure NoAnswerFrom(std::string_view endpoint)
{
    return SchedulerFailure { .endpoint = endpoint };
}

} // namespace

TEST_CASE("An hour of unanswered dials warns once and reminds once per cadence", "[node][scheduler][reachability]")
{
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };
    Tally said;

    for ([[maybe_unused]] auto const _: std::views::iota(0, Rounds))
    {
        ++said[reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level];
        clock.advance(Round);
    }

    // Dials at 0 s, 20 s, ... 3580 s. The loss is said once, at 0 s. A reminder is due a whole cadence
    // after the previous line above Debug: 600 s, 1200 s, ... 3000 s, five before 3580 s. Every other
    // dial is Debug. What this replaced was 180 Warn lines from this loop alone.
    constexpr auto Reminders = static_cast<std::size_t>(((Rounds - 1) * Round) / SchedulerUnreachableCadence);
    static_assert(Reminders == 5);
    CHECK(said[LogLevel::Warn] == 1);
    CHECK(said[LogLevel::Info] == Reminders);
    CHECK(said[LogLevel::Debug] == static_cast<std::size_t>(Rounds) - 1 - Reminders);

    // The recovery answers at the loss's level: Warn here.
    auto const back = reachability.Succeeded(AnnounceStage::Dial, SchedulerA);
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).level == LogLevel::Warn);
    CHECK(Unwrap(back).message == "scheduler scheduler-a.example:6676 reachable again after 3600s unreachable");
}

TEST_CASE("The first line of each outcome keeps the wording operators already read", "[node][scheduler][reachability]")
{
    // `docs/tools/fastcache-compile-node.md` quotes these, and the fallback suffix is what says where
    // the round went next (#1310). The level changed; the words did not. Each on its own scheduler, so
    // every one is a `Lost` at Warn rather than a `Further`.
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };

    auto const alone = reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA));
    CHECK(alone.level == LogLevel::Warn);
    CHECK(alone.message == "scheduler scheduler-a.example:6676 unreachable");

    auto const falling = reachability.Failed(
        SchedulerOutcome::Unreachable, SchedulerFailure { .endpoint = SchedulerB, .next = std::string { SchedulerA } });
    CHECK(falling.message == "scheduler scheduler-b.example:6676 unreachable; trying scheduler-a.example:6676");

    auto const refused = reachability.Failed(
        SchedulerOutcome::RegistrationRefused,
        SchedulerFailure { .endpoint = "c.example:6676", .subject = "a1b2c3", .reason = "rejected (not-a-member)" });
    CHECK(refused.level == LogLevel::Warn);
    CHECK(refused.message == "scheduler c.example:6676 did not register a1b2c3: rejected (not-a-member)");

    auto const unproved = reachability.Failed(
        SchedulerOutcome::IdentityRefused,
        SchedulerFailure { .endpoint = "d.example:6676", .reason = "node-key-unknown", .next = std::string { SchedulerA } });
    CHECK(unproved.message
          == "d.example:6676 did not accept this machine's identity: node-key-unknown; trying scheduler-a.example:6676");

    auto const unrecorded =
        reachability.Failed(SchedulerOutcome::PresenceRefused,
                            SchedulerFailure { .endpoint = "e.example:6676", .reason = "at 10.0.0.2:6677: not-a-member" });
    CHECK(unrecorded.message == "scheduler e.example:6676 did not record this machine at 10.0.0.2:6677: not-a-member");
}

TEST_CASE("A reminder is due a whole cadence after the last line and never in a burst", "[node][scheduler][reachability]")
{
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };
    REQUIRE(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Warn);

    clock.advance(SchedulerUnreachableCadence - std::chrono::seconds { 1 });
    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Debug);

    clock.advance(std::chrono::seconds { 1 });
    auto const reminder = reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA));
    CHECK(reminder.level == LogLevel::Info);
    CHECK(reminder.message == "scheduler scheduler-a.example:6676 unreachable -- still, after 600s");

    // Consumed: the next is a whole cadence from THIS line, so asking at once owes nothing.
    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Debug);
}

TEST_CASE("A flapping scheduler warns at most once per cadence", "[node][scheduler][reachability]")
{
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };
    Tally said;

    // An hour of a link that drops every other round: failing dials at 0 s, 40 s, ... 3560 s and
    // answering ones between. Each loss is a real transition, and saying every one at Warn is 90 Warns
    // an hour -- the flood this exists to stop, reached by a different door.
    for (auto const round: std::views::iota(0, Rounds))
    {
        if ((round % 2) == 0)
            ++said[reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level];
        else if (auto const back = reachability.Succeeded(AnnounceStage::Dial, SchedulerA); back.has_value())
            ++said[back->level];
        clock.advance(Round);
    }

    // A loss at Warn at 0 s and at the first loss a whole cadence after the previous one: 600 s, ...
    // 3000 s, six of them. Each is answered by its recovery at the same level. Every other
    // loss is a `Relapse` at Debug, and so is its recovery. Nothing is said at Info.
    constexpr auto Losses = static_cast<std::size_t>(((Rounds - 2) * Round) / SchedulerUnreachableCadence) + 1;
    static_assert(Losses == 6);
    CHECK(said[LogLevel::Warn] == 2 * Losses);
    CHECK(said[LogLevel::Info] == 0);
}

TEST_CASE("A change of outcome at one scheduler is a transition", "[node][scheduler][reachability]")
{
    // A scheduler that stops refusing this machine and becomes unreachable -- or the
    // reverse -- is news, never a repeat. It is said at Info rather than Warn while that scheduler
    // already had its Warn this cadence, and at Warn once the budget renews.
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };
    auto const refusedA = SchedulerFailure { .endpoint = SchedulerA, .reason = "node-key-unknown" };

    CHECK(reachability.Failed(SchedulerOutcome::IdentityRefused, refusedA).level == LogLevel::Warn);
    CHECK(reachability.Failed(SchedulerOutcome::IdentityRefused, refusedA).level == LogLevel::Debug);

    auto const changed = reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA));
    CHECK(changed.level == LogLevel::Info);
    CHECK(changed.message == "scheduler scheduler-a.example:6676 unreachable");

    clock.advance(SchedulerUnreachableCadence);
    CHECK(reachability.Failed(SchedulerOutcome::Untrusted, refusedA).level == LogLevel::Warn);
}

TEST_CASE("A success ends only the outcomes of its own stage and earlier ones", "[node][scheduler][reachability]")
{
    // A dial that connects says nothing about a proof the scheduler goes on refusing, so the outage
    // outlives it. The worker's heartbeat and the presence loop share this tracker, and the worker's
    // successful proof must not end the presence loop's refusal either.
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };

    REQUIRE(reachability
                .Failed(SchedulerOutcome::PresenceRefused,
                        SchedulerFailure { .endpoint = SchedulerA, .reason = "at 10.0.0.2:6677: no" })
                .level
            == LogLevel::Warn);
    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Dial, SchedulerA).has_value());
    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Proof, SchedulerA).has_value());

    auto const recorded = reachability.Succeeded(AnnounceStage::Announcement, SchedulerA);
    REQUIRE(recorded.has_value());
    CHECK(Unwrap(recorded).message.starts_with("scheduler scheduler-a.example:6676 recorded this machine again after"));

    // A registration is filed under its toolchain: one toolchain accepted ends nothing for another.
    auto const refusedGcc =
        SchedulerFailure { .endpoint = SchedulerB, .subject = "gcc-14", .reason = "rejected (malformed-registration)" };
    REQUIRE(reachability.Failed(SchedulerOutcome::RegistrationRefused, refusedGcc).level == LogLevel::Warn);
    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Announcement, SchedulerB, "clang-19").has_value());
    CHECK(reachability.Succeeded(AnnounceStage::Announcement, SchedulerB, "gcc-14").has_value());
}

TEST_CASE("An endpoint nobody dialled for a whole cadence is forgotten so its next failure is news",
          "[node][scheduler][reachability]")
{
    // `SchedulerLink::Lost` lets go of a remembered leader that stopped answering, and a PC that slept
    // for an hour dialled nothing either. "Still unreachable" about an address this process stopped
    // asking is a confident wrong signal, and so is "reachable again" about one it can no longer
    // account for.
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };
    REQUIRE(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Warn);

    clock.advance(SchedulerUnreachableCadence + std::chrono::seconds { 1 });
    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Dial, SchedulerA).has_value());

    // And a later failure is a fresh loss, at Warn -- not a relapse of a warning nobody saw recently.
    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Warn);
}

TEST_CASE("Schedulers are tracked apart", "[node][scheduler][reachability]")
{
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };

    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerA)).level == LogLevel::Warn);
    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerB)).level == LogLevel::Warn);

    auto const back = reachability.Succeeded(AnnounceStage::Dial, SchedulerA);
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).message.contains(SchedulerA));

    // B's outage is B's: A answering ends nothing for it.
    CHECK(reachability.Failed(SchedulerOutcome::Unreachable, NoAnswerFrom(SchedulerB)).level == LogLevel::Debug);
}

TEST_CASE("A scheduler that never went away says nothing when it answers", "[node][scheduler][reachability]")
{
    // The common case, every round of a healthy fleet: no line at all, at any level.
    core::platform::ManualClock clock;
    SchedulerReachability reachability { clock };

    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Announcement, SchedulerA).has_value());
}

TEST_CASE("Every event of every outcome formats and names its scheduler", "[node][scheduler][reachability]")
{
    // The patterns are runtime format strings, so a malformed one throws where it is first SAID --
    // on a node whose scheduler just went down. Walked here instead, every pair.
    auto const failure = SchedulerFailure {
        .endpoint = SchedulerA, .subject = "gcc-14", .reason = "why", .next = std::string { "x.example:1" }
    };
    for (auto const outcome: Enumerators<SchedulerOutcome>())
        for (auto const event: Enumerators<ReachabilityEvent>())
        {
            CAPTURE(static_cast<int>(outcome), static_cast<int>(event));
            auto said = RoundReport {};
            CHECK_NOTHROW(said = DescribeSetback(event, outcome, failure, std::chrono::seconds { 42 }, LogLevel::Warn));
            CHECK(said.level == ReachabilityRowOf(event).level.value_or(LogLevel::Warn));
            CHECK(said.message.contains(SchedulerA));
        }
}

TEST_CASE("Each unproved identity maps to its own outcome", "[node][scheduler][reachability]")
{
    CHECK(SchedulerOutcomeOfProof(NodeProofResult::NotOffered) == SchedulerOutcome::NoHandshake);
    CHECK(SchedulerOutcomeOfProof(NodeProofResult::Untrusted) == SchedulerOutcome::Untrusted);
    CHECK(SchedulerOutcomeOfProof(NodeProofResult::Refused) == SchedulerOutcome::IdentityRefused);
}

TEST_CASE("scheduler-unreachable is raised while a scheduler does not answer and cleared when it does",
          "[node][scheduler][reachability][conditions]")
{
    namespace Wire = FastCache::CompileCacheWire;
    core::platform::ManualClock clock;
    NodeConditions conditions;
    SchedulerReachability reachability { clock, &conditions };

    // Evaluated as the tracker is built: `clear`, never the `undecided` a wired node must not show.
    CHECK(conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Clear);

    (void) reachability.Failed(SchedulerOutcome::Unreachable, SchedulerFailure { .endpoint = SchedulerA });
    CHECK(conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Raised);

    // A REFUSAL does not raise it: that scheduler answered, and the remedy names the network.
    (void) reachability.Failed(SchedulerOutcome::IdentityRefused,
                               SchedulerFailure { .endpoint = SchedulerB, .reason = "no" });
    (void) reachability.Succeeded(AnnounceStage::Dial, SchedulerA);
    CHECK(conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Clear);

    // And the detail names every scheduler that is not answering.
    (void) reachability.Failed(SchedulerOutcome::Unreachable, SchedulerFailure { .endpoint = SchedulerA });
    (void) reachability.Failed(SchedulerOutcome::Unreachable, SchedulerFailure { .endpoint = SchedulerB });
    auto const rows = conditions.Snapshot();
    auto const* const row =
        core::findOrNull(rows, RowFor(NodeCondition::SchedulerUnreachable).id, &Wire::NodeConditionFields::id);
    REQUIRE(row != nullptr);
    CHECK(row->detail.contains(SchedulerA));
    CHECK(row->detail.contains(SchedulerB));

    // Forgotten after a cadence nothing touched it: a claim this process can no longer make is withdrawn.
    clock.advance(SchedulerUnreachableCadence + std::chrono::seconds { 1 });
    CHECK_FALSE(reachability.Succeeded(AnnounceStage::Dial, SchedulerA).has_value());
    CHECK(conditions.StateOf(NodeCondition::SchedulerUnreachable) == Wire::ConditionState::Clear);
}
