// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

TEST_CASE("Events are the differences between two probes, keyed and in table order", "[livestats]")
{
    LiveEventProbe before;
    before.members = { { .key = "n1", .detail = "n1" }, { .key = "n2", .detail = "n2" } };
    before.leadership = LiveFact { .key = "3|", .detail = "" };
    before.survey = LiveFact { .key = "1", .detail = "1 of 4" };

    auto after = before;
    CHECK(DiffLiveEvents(before, after).empty());

    // The words changed and the key did not: not an event, or a survey would be one per toolchain.
    after.survey = LiveFact { .key = "1", .detail = "3 of 4" };
    CHECK(DiffLiveEvents(before, after).empty());

    after.members = { { .key = "n2", .detail = "n2" }, { .key = "n3", .detail = "n3" } };
    after.leadership = LiveFact { .key = "1|n3:6674", .detail = "n3:6674" };
    after.enrollment = LiveFact { .key = "2", .detail = "0 pending" };

    auto const events = DiffLiveEvents(before, after);
    REQUIRE(events.size() == 4);
    CHECK(events[0].kind == Wire::LiveEventKind::MemberJoined);
    CHECK(events[0].detail == "n3");
    CHECK(events[1].kind == Wire::LiveEventKind::MemberLeft);
    CHECK(events[1].detail == "n1");
    CHECK(events[2].kind == Wire::LiveEventKind::LeadershipChanged);
    CHECK(events[2].detail == "n3:6674");
    // Appearing is a change: a window that did not exist and now does.
    CHECK(events[3].kind == Wire::LiveEventKind::EnrollmentChanged);
}

TEST_CASE("A gap is whole cadences missed, never wake-up jitter", "[livestats]")
{
    LiveCursor cursor { .nextDue = 10, .ticksPerCadence = 2 };

    CHECK_FALSE(DecideLiveStep(cursor, 9, false).snapshot);

    auto const onTime = DecideLiveStep(cursor, 10, false);
    CHECK(onTime.snapshot);
    CHECK_FALSE(onTime.gap.has_value());
    CHECK(cursor.nextDue == 12);

    // One tick late is less than a cadence: jitter, and no hole in the panel.
    auto const jitter = DecideLiveStep(cursor, 13, false);
    CHECK(jitter.snapshot);
    CHECK_FALSE(jitter.gap.has_value());
    CHECK(cursor.nextDue == 15);

    auto const parked = DecideLiveStep(cursor, 21, false);
    CHECK(parked.snapshot);
    REQUIRE(parked.gap.has_value());
    CHECK(Unwrap(parked.gap).dropped == 3);
    CHECK(Unwrap(parked.gap).firstTick == 15);
    CHECK(Unwrap(parked.gap).lastTick == 19);

    // An event owes a snapshot whatever the cadence says, and restarts the cadence from there.
    auto const forced = DecideLiveStep(cursor, 22, true);
    CHECK(forced.snapshot);
    CHECK_FALSE(forced.gap.has_value());
    CHECK(cursor.nextDue == 24);
}

namespace
{

/// Sources whose every capture is numbered in its body, and which can run something WHILE a
/// capture is being taken -- which is how a case stands a second reactor inside the first's capture.
class NumberedSources final: public ILiveStatsSources
{
  public:
    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject /*subject*/) const override
    {
        ++captures;
        if (auto const during = std::exchange(duringNextCapture, nullptr); during)
            during();
        return LiveCapture { .body = { static_cast<std::byte>(captures) }, .probe = {} };
    }

    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "cache.test:6380";
    }

    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = {} });
    }

    mutable std::size_t captures { 0 };                 ///< Captures taken.
    mutable std::function<void()> duringNextCapture {}; ///< Run inside the next capture, once.
};

/// A gate that admits everyone, always.
class OpenGate final: public ILiveGate
{
  public:
    [[nodiscard]] std::optional<std::vector<std::byte>> RefuseWatcher(std::string_view /*peer*/) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> Admit(Wire::SubscribeRequest const& /*request*/,
                                                              std::string_view /*peer*/) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> Recheck(Wire::LiveSubject /*subject*/,
                                                                std::string_view /*peer*/) const override
    {
        return std::nullopt;
    }
};

/// A sink that delivers every push at once and keeps the snapshots' bodies by tick.
class SnapshotSink final: public IPushSink
{
  public:
    [[nodiscard]] Task<PushOutcome> Push(std::vector<std::byte> frame, std::chrono::milliseconds /*hold*/) override
    {
        auto const push = Wire::DecodePush(Testing::PayloadOf(frame));
        if (push.has_value() && push->kind == Wire::PushKind::Snapshot)
            if (auto const snapshot = Wire::DecodeLiveSnapshot(push->fields); snapshot.has_value())
                snapshots.emplace_back(snapshot->tick, std::vector<std::byte>(snapshot->body.begin(), snapshot->body.end()));
        co_return PushOutcome::Delivered;
    }

    [[nodiscard]] PeerActivity Activity() const noexcept override
    {
        return PeerActivity::Quiet;
    }

    [[nodiscard]] bool Stopping() const noexcept override
    {
        return stopping;
    }

    std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> snapshots; ///< (tick, body), in order.
    bool stopping { false };                                                 ///< Whether the surface is stopping.
};

/// Serve one cache subscription, recording that it returned.
DetachedTask ServeCache(LiveStream* live, SnapshotSink* sink, ILiveGate const* gate, IReactor* reactor, bool* returned)
{
    auto const frame = Wire::EncodeSubscribeRequest(
        Wire::SubscribeRequest { .subject = Wire::LiveSubject::Cache, .cadenceMillis = 500, .dashboardToken = {} });
    (void) co_await live->Serve(frame, "10.0.0.7", sink, gate, reactor);
    *returned = true;
}

/// Two reactors on one clock, each with one subscriber of one stream; stopped and drained on the way out,
/// so an assertion that unwinds a case leaves nothing parked.
struct TwoReactors
{
    TwoReactors() = default;
    TwoReactors(TwoReactors const&) = delete;
    TwoReactors(TwoReactors&&) = delete;
    TwoReactors& operator=(TwoReactors const&) = delete;
    TwoReactors& operator=(TwoReactors&&) = delete;

    ~TwoReactors()
    {
        first.stopping = true;
        second.stopping = true;
        clock.Advance(2s);
        firstReactor.Drain();
        secondReactor.Drain();
    }

    ManualClock clock;
    TestReactor firstReactor { clock };
    TestReactor secondReactor { clock };
    AtomicMetricsSink metrics;
    NumberedSources sources;
    OpenGate gate;
    LiveStream live { sources, metrics };
    SnapshotSink first;
    SnapshotSink second;
    bool firstReturned { false };
    bool secondReturned { false };
};

} // namespace

TEST_CASE("A capture another reactor is taking is not waited for: the subscriber looks again and sends that capture",
          "[livestats]")
{
    TwoReactors rig;
    ServeCache(&rig.live, &rig.first, &rig.gate, &rig.firstReactor, &rig.firstReturned);
    ServeCache(&rig.live, &rig.second, &rig.gate, &rig.secondReactor, &rig.secondReturned);
    rig.firstReactor.Drain();
    rig.secondReactor.Drain();
    REQUIRE(rig.sources.captures == 1); // Tick 0, taken once and shared.
    REQUIRE(rig.second.snapshots.size() == 1);

    // The first reactor claims tick 1; while its capture is being taken, the second reactor's
    // subscriber comes due. It must neither wait for that capture nor take one of its own.
    rig.sources.duringNextCapture = [&rig] {
        rig.secondReactor.Drain();
        CHECK(rig.second.snapshots.size() == 1); // Deferred: nothing sent from the capture in progress.
    };
    rig.clock.Advance(500ms);
    rig.firstReactor.Drain();
    CHECK(rig.sources.captures == 2);
    REQUIRE(rig.first.snapshots.size() == 2);
    CHECK(rig.first.snapshots.back().first == 1);

    // One stop-check later it reads what the first reactor published: tick 1's body, not tick 0's.
    rig.clock.Advance(LiveStopCheck);
    rig.secondReactor.Drain();
    CHECK(rig.sources.captures == 2);
    REQUIRE(rig.second.snapshots.size() == 2);
    CHECK(rig.second.snapshots.back().first == 1);
    CHECK(rig.second.snapshots.back().second == rig.first.snapshots.back().second);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSnapshotsRendered) == 2);
}

TEST_CASE("A subscriber that finds the next capture claimed sends the newest one it has not read, labelled with its tick",
          "[livestats]")
{
    // WHAT DISTINGUISHES: the first reactor's capture of tick 1 is published while the second reactor's subscriber
    // sleeps, and the first reactor has claimed tick 2 by the time it looks. A subscriber that waited for the claimed
    // capture would find it claimed on every look for as long as captures are slower than a tick -- the starvation a
    // real slow capture produces -- and would send nothing from inside that capture. The one that sends what was
    // published sends tick 1 there, under tick 1's label, and then tick 2 once that is published.
    TwoReactors rig;
    ServeCache(&rig.live, &rig.first, &rig.gate, &rig.firstReactor, &rig.firstReturned);
    ServeCache(&rig.live, &rig.second, &rig.gate, &rig.secondReactor, &rig.secondReturned);
    rig.firstReactor.Drain();
    rig.secondReactor.Drain();
    REQUIRE(rig.second.snapshots.size() == 1);

    // Tick 1: the second reactor looks inside the first's capture and has read everything published.
    rig.sources.duringNextCapture = [&rig] {
        rig.secondReactor.Drain();
    };
    rig.clock.Advance(500ms);
    rig.firstReactor.Drain();
    REQUIRE(rig.second.snapshots.size() == 1);
    REQUIRE(rig.first.snapshots.size() == 2);

    // Tick 2: claimed by the first reactor before the second looks again.
    auto sentInsideCapture = std::vector<std::uint64_t> {};
    rig.sources.duringNextCapture = [&rig, &sentInsideCapture] {
        rig.secondReactor.Drain();
        for (auto const& [tick, body]: rig.second.snapshots)
            sentInsideCapture.push_back(tick);
    };
    rig.clock.Advance(500ms);
    rig.firstReactor.Drain();
    CHECK(sentInsideCapture == std::vector<std::uint64_t> { 0, 1 });
    CHECK(rig.sources.captures == 3);

    rig.clock.Advance(LiveStopCheck);
    rig.secondReactor.Drain();
    REQUIRE(rig.second.snapshots.size() == 3);
    CHECK(rig.second.snapshots[1].second == rig.first.snapshots[1].second);
    CHECK(rig.second.snapshots.back().first == 2);
    CHECK(rig.second.snapshots.back().second == rig.first.snapshots.back().second);
    CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSnapshotsRendered) == 3);
}
