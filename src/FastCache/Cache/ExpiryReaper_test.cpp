// SPDX-License-Identifier: Apache-2.0
//
// The active expiry cycle. The case that matters is the one nothing else
// covers: a key whose TTL lapses and which no client ever touches again. Every
// other reclamation path in this codebase is driven by a call naming the key,
// so without a timer that key is never freed and its `expired` event is never
// published -- which is precisely the case a subscriber subscribes for.
#include <FastCache/Async/Cancellation.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;
using FastCache::Testing::MakeBytes;

namespace
{

using namespace FastCache;

/// Observer that keeps what it is told, so a case can assert on the event
/// stream a subscriber would have seen rather than on storage state alone.
/// Reclamation being observable is half of what this feature is for.
class RecordingObserver final: public IStorageMutationObserver
{
  public:
    void OnMutation(MutationKind kind, std::string_view key) noexcept override
    {
        events.emplace_back(std::format("{}:{}", static_cast<int>(kind), key));
    }

    [[nodiscard]] bool HasObservers() const noexcept override
    {
        return true;
    }

    /// @return True if `kind` was reported for `key`.
    [[nodiscard]] bool Saw(MutationKind kind, std::string_view key) const
    {
        return std::ranges::contains(events, std::format("{}:{}", static_cast<int>(kind), key));
    }

    std::vector<std::string> events;
};

/// Everything one case needs, wired the way the daemon wires it: the reclaim
/// log below the notifying decorator, so a key the sweep reclaims becomes an
/// event rather than just a smaller item count.
struct Fixture
{
    RecordingObserver observer;
    ReclaimLog log { &observer };
    InMemoryLruStorage lru;
    NotifyingStorage storage { lru, &observer };
    ManualClock clock;
    TestReactor reactor { clock };
    NullLogger logger;
    CancellationSource source;
    AtomicMetricsSink metrics;

    Fixture()
    {
        storage.SetReclaimLog(&log);
    }
};

} // namespace

TEST_CASE("The expiry cycle reclaims a lapsed key nobody touched, and says so", "[expiry][reaper]")
{
    // The issue's own reproduction, one layer down from the wire: SET with a
    // TTL, let it lapse, touch NOTHING. Before the cycle existed the entry
    // stayed resident and no `expired` event was ever published for it.
    Fixture f;
    REQUIRE(f.storage.Set("gone", MakeBytes("v"), 0, f.clock.Now() + 1s).has_value());
    f.observer.events.clear(); // Drop the SET's own event.

    ExpiryReaper reaper {
        f.storage, f.logger, ExpiryReaperOptions { .interval = 100ms, .stopWakeBound = 25ms }, &f.metrics
    };
    auto task = reaper.Run(f.reactor, f.source.Token());
    f.reactor.Submit(task.Native());
    f.reactor.Drain();
    CHECK(f.storage.Snapshot().itemCount == 1U); // Still live; nothing to do yet.

    f.clock.Advance(2s);
    f.reactor.Drain();

    CHECK(f.storage.Snapshot().itemCount == 0U);
    CHECK(f.observer.Saw(MutationKind::Expire, "gone"));
    CHECK(f.metrics.Read(IMetricsSink::Counter::ExpiryKeysReclaimed) == 1U);
    CHECK(f.metrics.Read(IMetricsSink::Counter::ExpiryCycles) >= 1U);

    f.source.Cancel();
    f.clock.Advance(25ms);
    f.reactor.Drain();
}

TEST_CASE("The expiry cycle stops promptly and leaves nothing parked", "[expiry][reaper]")
{
    // A periodic task on a reactor is a coroutine frame parked on the timer
    // wheel between turns. If the wait could not be interrupted, a stop would
    // either wait out the whole interval or return with a frame nobody will
    // ever resume and nobody will ever free.
    Fixture f;
    ExpiryReaper reaper { f.storage, f.logger, ExpiryReaperOptions { .interval = 30s, .stopWakeBound = 50ms } };
    auto task = reaper.Run(f.reactor, f.source.Token());
    f.reactor.Submit(task.Native());
    f.reactor.Drain();

    auto const started = f.clock.Now();
    f.source.Cancel();
    f.clock.Advance(50ms);
    f.reactor.Drain();

    CHECK(f.clock.Now() - started == 50ms); // Not the 30s interval.
    CHECK(f.reactor.PendingTimers() == 0);
    CHECK(f.reactor.PendingSubmissions() == 0);
    CHECK(reaper.Cycles() == 0U);
}

TEST_CASE("Stopping the expiry cycle reclaims a frame still parked on the reactor", "[expiry][reaper]")
{
    // The case a graceful shutdown does not cover: the loop stops while the
    // cycle is asleep between sweeps. Nothing will ever resume that frame, so
    // unless its owner takes it back off the timer wheel it is leaked -- and a
    // sanitizer build reports it as exactly that.
    Fixture f;
    {
        ExpiryReaper reaper { f.storage, f.logger, ExpiryReaperOptions { .interval = 30s, .stopWakeBound = 50ms } };
        reaper.Start(f.reactor);
        f.reactor.Drain();
        REQUIRE(f.reactor.PendingTimers() == 1); // Parked mid-interval.
    } // ~ExpiryReaper -> Stop() -> CancelPending, then the frame is destroyed.

    CHECK(f.reactor.PendingTimers() == 0);
    CHECK(f.reactor.PendingSubmissions() == 0);
}

TEST_CASE("A zero interval disables the expiry cycle rather than parking it", "[expiry][reaper]")
{
    // "Off" has to mean a coroutine that ended. One parked forever on a
    // deadline nothing will move is a frame the reactor has to outlive.
    Fixture f;
    REQUIRE(f.storage.Set("gone", MakeBytes("v"), 0, f.clock.Now() + 1s).has_value());

    ExpiryReaper reaper { f.storage, f.logger, ExpiryReaperOptions { .interval = Duration::zero() } };
    auto task = reaper.Run(f.reactor, f.source.Token());
    f.reactor.Submit(task.Native());
    f.reactor.Drain();

    f.clock.Advance(1h);
    f.reactor.Drain();

    CHECK(reaper.Cycles() == 0U);
    CHECK(f.reactor.PendingTimers() == 0);
    CHECK(f.storage.Snapshot().itemCount == 1U); // Expiry stays purely access-driven.
}

TEST_CASE("The expiry cycle backs off while idle and comes straight back when it finds work", "[expiry][reaper]")
{
    // A cache with nothing to expire should not keep paying the base interval
    // to be told so -- but the moment there is something, the next sweep must
    // not be a backed-off one.
    constexpr auto Base = 100ms;
    constexpr auto Ceiling = 400ms;
    ExpiryReaperOptions const options { .interval = Base, .maxInterval = Ceiling };

    // Idle: a completed pass that reclaimed nothing doubles, up to the ceiling.
    CHECK(NextExpiryInterval(options, Base, PurgeOutcome { .completedPass = true }) == 200ms);
    CHECK(NextExpiryInterval(options, 200ms, PurgeOutcome { .completedPass = true }) == Ceiling);
    CHECK(NextExpiryInterval(options, Ceiling, PurgeOutcome { .completedPass = true }) == Ceiling);

    // Work reclaimed: straight back to the base interval, however long the
    // cycle had backed off to.
    CHECK(NextExpiryInterval(options, Ceiling, PurgeOutcome { .purged = 1, .completedPass = true }) == Base);

    // Out of budget is also work: there are entries this pass never examined,
    // so backing off here would be backing off from a cache full of them.
    CHECK(NextExpiryInterval(options, Ceiling, PurgeOutcome { .scanned = 8, .completedPass = false }) == Base);
}

TEST_CASE("The expiry cycle actually backs off on a running reactor", "[expiry][reaper]")
{
    // The pure function above decides the interval; this is the check that the
    // loop uses what it decides.
    Fixture f;
    ExpiryReaper reaper { f.storage,
                          f.logger,
                          ExpiryReaperOptions { .interval = 100ms, .maxInterval = 400ms, .stopWakeBound = 100ms } };
    auto task = reaper.Run(f.reactor, f.source.Token());
    f.reactor.Submit(task.Native());
    f.reactor.Drain();
    CHECK(reaper.CurrentInterval() == 100ms);

    // Three idle sweeps: 100 -> 200 -> 400, then held at the ceiling.
    for (auto const expected: { 200ms, 400ms, 400ms })
    {
        f.clock.Advance(500ms);
        f.reactor.Drain();
        CHECK(reaper.CurrentInterval() == expected);
    }

    // Something to reclaim, and the very next sweep is back at the base.
    REQUIRE(f.storage.Set("gone", MakeBytes("v"), 0, f.clock.Now() + 1s).has_value());
    f.clock.Advance(2s);
    f.reactor.Drain();
    CHECK(reaper.CurrentInterval() == 100ms);
    CHECK(f.observer.Saw(MutationKind::Expire, "gone"));

    f.source.Cancel();
    f.clock.Advance(100ms);
    f.reactor.Drain();
}

TEST_CASE("One expiry sweep spends no more than its budget", "[expiry][reaper]")
{
    // The ceiling is what makes a periodic sweep affordable: without it the
    // cost of every cycle is proportional to how much is cached, under the
    // tier's exclusive lock.
    Fixture f;
    for (auto const i: std::views::iota(0, 10))
        REQUIRE(f.storage.Set(std::format("k-{}", i), MakeBytes("v"), 0, f.clock.Now() + 1s).has_value());
    f.clock.Advance(2s);

    ExpiryReaper reaper { f.storage, f.logger, ExpiryReaperOptions { .scanBudget = 4, .purgeBudget = 3 } };

    auto const first = reaper.SweepOnce(f.clock.Now());
    CHECK(first.purged == 3U); // The reclaim ceiling bites before the scan one.
    CHECK_FALSE(first.completedPass);
    CHECK(f.storage.Snapshot().itemCount == 7U);

    // Three more clear the remaining seven -- three, three, one -- and a fourth
    // finds nothing, which is what the cycle would then back off on.
    for ([[maybe_unused]] auto const step: std::views::iota(0, 4))
        std::ignore = reaper.SweepOnce(f.clock.Now());
    CHECK(f.storage.Snapshot().itemCount == 0U);
    CHECK(reaper.Cycles() == 5U);
}
