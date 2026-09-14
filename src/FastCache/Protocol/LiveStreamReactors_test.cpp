// SPDX-License-Identifier: Apache-2.0
//
// One `LiveStream` served on two REAL reactors, the shape `fastcached --threads 2` runs (#1399).
//
// Every other live-stats case drives one `TestReactor`, where one thread does everything and no
// subscriber can wait on another. The daemon shares ONE stream across all of its reactors -- the
// per-tick capture cache behind one mutex, the cap behind one atomic -- so what those cases cannot
// see is what one reactor's subscriber costs another's: a lock held across work on one thread is a
// reactor thread blocked on the other. These cases run each reactor on its own thread against the
// real clock and measure it: every reactor carries a heartbeat whose lateness is how long its
// thread could not run.

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

/// The cache subject's floor, which is its tick: every expectation below is counted in these.
constexpr auto Floor = Wire::LiveSubjectTable[static_cast<std::size_t>(Wire::LiveSubject::Cache)].floor;

/// The cache subject's sources, with a capture that can be made to take a while, and that counts.
///
/// The delay stands for what a daemon's capture actually waits on: `engine.Snapshot()` takes every
/// shard's lock in turn, so a capture lasts as long as the longest write holding one. It can be
/// confined to ONE thread, so a case can tell a reactor waiting on another's capture from a reactor
/// running a slow capture of its own.
class TimedSources final: public ILiveStatsSources
{
  public:
    /// @param metrics The counters a capture reads.
    /// @param delay How long every capture takes.
    TimedSources(IMetricsSink const& metrics, std::chrono::milliseconds delay) noexcept:
        _metrics { metrics },
        _delay { delay }
    {
    }

    [[nodiscard]] std::optional<LiveCapture> Capture(Wire::LiveSubject /*subject*/) const override
    {
        _captures.fetch_add(1, std::memory_order_acq_rel);
        auto const slowOn = _slowOn.load(std::memory_order_acquire);
        if (slowOn == std::thread::id {} || slowOn == std::this_thread::get_id())
            std::this_thread::sleep_for(_delay);
        auto snapshot = MetricsSnapshot {};
        snapshot.storage = StorageStats { .itemCount = 7 };
        return CaptureCacheSubject(_metrics, snapshot);
    }

    [[nodiscard]] std::optional<LiveLeadership> Leadership() const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::string AnsweringEndpoint() const override
    {
        return "races.test:6380";
    }

    [[nodiscard]] std::expected<FleetTextDocument, FleetTextDeclined> FleetText(std::string_view /*section*/,
                                                                                std::string_view /*range*/) const override
    {
        return std::unexpected(FleetTextDeclined { .refusal = FleetTextRefusal::NoFleet, .detail = {} });
    }

    /// Take the delay only on @p thread from now on; every other thread captures at once.
    /// @param thread The one slow thread.
    void SlowOn(std::thread::id thread) noexcept
    {
        _slowOn.store(thread, std::memory_order_release);
    }

    /// @return How many captures began.
    [[nodiscard]] std::size_t Captures() const noexcept
    {
        return _captures.load(std::memory_order_acquire);
    }

  private:
    IMetricsSink const& _metrics;
    std::chrono::milliseconds _delay;
    std::atomic<std::thread::id> _slowOn {};
    mutable std::atomic<std::size_t> _captures { 0 };
};

/// A gate that admits everyone, on every tick: who is admitted is not what these cases are about.
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

/// One subscriber's surface: records every frame it is handed, and optionally parks each snapshot
/// push on its reactor for a while, as a watcher that reads slowly does.
///
/// A parked push is a SleepUntil on the subscriber's own reactor, never a blocked thread: what a
/// slow watcher costs in production is a write that is not complete yet, and the reactor goes on
/// serving meanwhile. So any thread that stops running while one is parked was stopped by the
/// stream, not by this sink.
class RecordingSink final: public IPushSink
{
  public:
    /// @param reactor The reactor this subscriber's connection runs on.
    /// @param park How long each snapshot push stays parked; zero delivers at once.
    /// @param stopping Set when the case ends every stream.
    RecordingSink(IReactor& reactor, std::chrono::milliseconds park, std::atomic<bool> const& stopping) noexcept:
        _reactor { reactor },
        _park { park },
        _stopping { stopping }
    {
    }

    [[nodiscard]] Task<PushOutcome> Push(std::vector<std::byte> frame, std::chrono::milliseconds /*hold*/) override
    {
        // The surface is the one writer, so every push reaches it on its connection's reactor.
        if (!_reactor.IsOnWorkerThread())
            _offReactor.fetch_add(1, std::memory_order_acq_rel);
        auto const snapshot = IsSnapshot(frame);
        {
            std::scoped_lock const guard { _mutex };
            _frames.push_back(std::move(frame));
        }
        if (snapshot && _park > 0ms)
            co_await SleepUntil { .reactor = &_reactor, .deadline = _reactor.Clock().Now() + _park };
        co_return PushOutcome::Delivered;
    }

    [[nodiscard]] PeerActivity Activity() const noexcept override
    {
        return PeerActivity::Quiet;
    }

    [[nodiscard]] bool Stopping() const noexcept override
    {
        return _stopping.load(std::memory_order_acquire);
    }

    /// @return Every frame pushed so far.
    [[nodiscard]] std::vector<std::vector<std::byte>> Frames() const
    {
        std::scoped_lock const guard { _mutex };
        return _frames;
    }

    /// @return How many pushes arrived on a thread that was not this subscriber's reactor.
    [[nodiscard]] std::size_t OffReactor() const noexcept
    {
        return _offReactor.load(std::memory_order_acquire);
    }

  private:
    /// @return True when @p frame is a snapshot push.
    [[nodiscard]] static bool IsSnapshot(std::span<std::byte const> frame)
    {
        auto const header = Wire::DecodeReplyHeader(frame);
        if (!header.has_value() || header->status != Wire::Status::Push)
            return false;
        auto const push = Wire::DecodePush(frame.subspan(Wire::ReplyHeaderSize));
        return push.has_value() && push->kind == Wire::PushKind::Snapshot;
    }

    IReactor& _reactor;
    std::chrono::milliseconds _park;
    std::atomic<bool> const& _stopping;
    mutable std::mutex _mutex;
    std::vector<std::vector<std::byte>> _frames;
    std::atomic<std::size_t> _offReactor { 0 };
};

/// How long a reactor's thread could not run, measured from inside it.
///
/// A coroutine sleeps a short step at a time and notes how late each wake-up was. A reactor whose
/// thread is blocked -- on a lock, on a capture -- wakes it late by the length of the block, and
/// nothing else on an idle reactor can.
struct Heartbeat
{
    std::atomic<std::int64_t> worstMicros { 0 }; ///< The latest wake-up so far.
    std::atomic<std::size_t> beats { 0 };        ///< Wake-ups so far, so a heartbeat that never ran is visible.
    std::atomic<std::thread::id> thread {};      ///< The reactor's thread, as the heartbeat found it.
};

/// The step a heartbeat sleeps.
constexpr auto HeartbeatStep = 20ms;

/// Beat on @p reactor until @p stopping.
/// @param reactor Where to beat.
/// @param heartbeat What to record into.
/// @param stopping When to end.
DetachedTask Beat(IReactor* reactor, Heartbeat* heartbeat, std::atomic<bool> const* stopping)
{
    co_await ResumeOn { *reactor };
    heartbeat->thread.store(std::this_thread::get_id(), std::memory_order_release);
    while (!stopping->load(std::memory_order_acquire))
    {
        auto const deadline = reactor->Clock().Now() + HeartbeatStep;
        co_await SleepUntil { .reactor = reactor, .deadline = deadline };
        auto const late = std::chrono::duration_cast<std::chrono::microseconds>(reactor->Clock().Now() - deadline).count();
        auto worst = heartbeat->worstMicros.load(std::memory_order_acquire);
        while (late > worst && !heartbeat->worstMicros.compare_exchange_weak(worst, late))
        {
        }
        heartbeat->beats.fetch_add(1, std::memory_order_acq_rel);
    }
}

/// What one subscription left behind once its stream returned.
struct Ended
{
    std::atomic<bool> returned { false }; ///< The stream's task completed.
};

/// Serve one cache subscription on @p reactor.
/// @param live The stream.
/// @param reactor Where the subscriber's connection runs.
/// @param sink Its surface.
/// @param gate Who is admitted.
/// @param ended Set when the stream returns.
DetachedTask Subscribe(LiveStream* live, IReactor* reactor, RecordingSink* sink, OpenGate const* gate, Ended* ended)
{
    co_await ResumeOn { *reactor };
    auto const frame = Wire::EncodeSubscribeRequest(
        Wire::SubscribeRequest { .subject = Wire::LiveSubject::Cache, .cadenceMillis = 0, .dashboardToken = {} });
    (void) co_await live->Serve(frame, "127.0.0.1", sink, gate, reactor);
    ended->returned.store(true, std::memory_order_release);
}

/// A reactor on its own thread, stopped and joined before it is destroyed on every way out of a case.
struct ReactorThread
{
    SteadyClock clock;
    PlatformReactor reactor { clock };
    std::thread worker { [this] { reactor.Run(); } };

    ReactorThread() = default;
    ReactorThread(ReactorThread const&) = delete;
    ReactorThread(ReactorThread&&) = delete;
    ReactorThread& operator=(ReactorThread const&) = delete;
    ReactorThread& operator=(ReactorThread&&) = delete;

    ~ReactorThread()
    {
        // Joined before `reactor` goes, which member order alone would not give: this body runs
        // first. Anything still parked on it is freed with it, and owns what that touches.
        reactor.Stop();
        worker.join();
    }
};

/// A pushed frame, decoded.
struct Decoded
{
    Wire::PushKind kind { Wire::PushKind::Subscribed }; ///< What it is.
    std::uint64_t tick { 0 };                           ///< A snapshot's tick.
    std::uint64_t dropped { 0 };                        ///< A gap's cadences.
};

/// Decode every frame one subscriber was handed, failing the case on any that does not decode as a
/// client would read it: a push, and for a snapshot a `StatsReading` in this build's layout.
/// @param frames The frames.
/// @return Them, decoded.
[[nodiscard]] std::vector<Decoded> DecodeAll(std::vector<std::vector<std::byte>> const& frames)
{
    auto decoded = std::vector<Decoded> {};
    for (auto const& frame: frames)
    {
        auto const header = Wire::DecodeReplyHeader(frame);
        REQUIRE(header.has_value());
        REQUIRE(Unwrap(header).status == Wire::Status::Push);
        auto const push = Wire::DecodePush(std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize));
        REQUIRE(push.has_value());
        auto const& fields = Unwrap(push).fields;
        auto one = Decoded { .kind = Unwrap(push).kind };
        switch (one.kind)
        {
            case Wire::PushKind::Subscribed: {
                auto const granted = Wire::DecodeLiveSubscribed(fields);
                REQUIRE(granted.has_value());
                CHECK(Unwrap(granted).statsLayout == StatsReadingLayout);
                break;
            }
            case Wire::PushKind::Snapshot: {
                auto const snapshot = Wire::DecodeLiveSnapshot(fields);
                REQUIRE(snapshot.has_value());
                auto const reading = DecodeStatsReading(Unwrap(snapshot).body);
                REQUIRE(reading.has_value());
                auto const& storage = reading.value().snapshot.storage;
                REQUIRE(storage.has_value());
                CHECK(Unwrap(storage).itemCount == 7U);
                one.tick = Unwrap(snapshot).tick;
                break;
            }
            case Wire::PushKind::Gap: {
                auto const gap = Wire::DecodeLiveGap(fields);
                REQUIRE(gap.has_value());
                one.dropped = Unwrap(gap).dropped;
                break;
            }
            case Wire::PushKind::Event:
                FAIL("a cache subscription is handed no events: the cache subject diffs none");
        }
        decoded.push_back(one);
    }
    return decoded;
}

/// The snapshot ticks in @p frames, in the order they were pushed.
[[nodiscard]] std::vector<std::uint64_t> TicksOf(std::vector<Decoded> const& frames)
{
    auto ticks = std::vector<std::uint64_t> {};
    for (auto const& frame: frames)
        if (frame.kind == Wire::PushKind::Snapshot)
            ticks.push_back(frame.tick);
    return ticks;
}

/// How many gaps @p frames carry.
[[nodiscard]] std::size_t GapsOf(std::vector<Decoded> const& frames)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(frames, [](Decoded const& frame) { return frame.kind == Wire::PushKind::Gap; }));
}

/// How many frames of @p kind @p frames hold, for a wait's predicate: never fails the case, since what a predicate
/// sees mid-stream is decoded -- and asserted -- once the stream has returned.
/// @param frames The frames so far.
/// @param kind The kind to count.
/// @return The count.
[[nodiscard]] std::size_t CountOf(std::vector<std::vector<std::byte>> const& frames, Wire::PushKind kind)
{
    return static_cast<std::size_t>(std::ranges::count_if(frames, [kind](std::vector<std::byte> const& frame) {
        auto const header = Wire::DecodeReplyHeader(frame);
        if (!header.has_value() || header->status != Wire::Status::Push)
            return false;
        auto const push = Wire::DecodePush(std::span<std::byte const> { frame }.subspan(Wire::ReplyHeaderSize));
        return push.has_value() && push->kind == kind;
    }));
}

/// Wait on the real clock until @p predicate holds or @p bound passes.
/// @return How long it waited, and whether it held.
template <typename Predicate>
[[nodiscard]] std::pair<std::chrono::milliseconds, bool> WaitFor(std::chrono::milliseconds bound, Predicate predicate)
{
    auto const start = std::chrono::steady_clock::now();
    while (!predicate() && std::chrono::steady_clock::now() - start < bound)
        std::this_thread::sleep_for(10ms);
    return { std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start), predicate() };
}

/// Two subscribers of one stream, each on its own real reactor, and a heartbeat on each.
///
/// Declared so that everything a parked frame touches outlives the reactors that free it: the
/// stream, the sources, the gate, the sinks and the records come first, the reactors last.
struct TwoReactorRig
{
    /// @param captureDelay How long each capture takes.
    /// @param parkFirst How long each of the FIRST subscriber's snapshot pushes stays parked.
    TwoReactorRig(std::chrono::milliseconds captureDelay, std::chrono::milliseconds parkFirst):
        sources { metrics, captureDelay }
    {
        // Built in the body, once the reactors exist, and declared before them: a sink must outlive every thread
        // that may still push into it.
        sinks[0] = std::make_unique<RecordingSink>(reactors[0].reactor, parkFirst, stopping);
        sinks[1] = std::make_unique<RecordingSink>(reactors[1].reactor, 0ms, stopping);
    }

    TwoReactorRig(TwoReactorRig const&) = delete;
    TwoReactorRig(TwoReactorRig&&) = delete;
    TwoReactorRig& operator=(TwoReactorRig const&) = delete;
    TwoReactorRig& operator=(TwoReactorRig&&) = delete;

    ~TwoReactorRig()
    {
        // Every stream sees the stop within `LiveStopCheck` and returns; a bounded wait, and the
        // reactors free whatever did not.
        stopping.store(true, std::memory_order_release);
        (void) StreamsReturned(10s);
    }

    /// Start a heartbeat on each reactor.
    void StartHeartbeats()
    {
        for (auto const index: { 0U, 1U })
            Beat(&reactors[index].reactor, &heartbeats[index], &stopping);
    }

    /// Start the subscriber on reactor @p index.
    /// @param index Which reactor.
    void StartSubscriber(std::size_t index)
    {
        started.at(index) = true;
        Subscribe(&live, &reactors.at(index).reactor, sinks.at(index).get(), &gate, &ended.at(index));
    }

    /// Start both heartbeats and both subscribers together.
    void Start()
    {
        StartHeartbeats();
        StartSubscriber(0);
        StartSubscriber(1);
    }

    /// Wait for every subscriber started to have returned.
    /// @param bound How long to wait.
    /// @return Whether they all did.
    [[nodiscard]] bool StreamsReturned(std::chrono::milliseconds bound)
    {
        auto const all = [this] {
            return std::ranges::all_of(std::views::iota(std::size_t { 0 }, started.size()), [this](std::size_t index) {
                return !started.at(index) || ended.at(index).returned.load();
            });
        };
        return WaitFor(bound, all).second;
    }

    AtomicMetricsSink metrics;
    std::atomic<bool> stopping { false };
    TimedSources sources;
    OpenGate const gate;
    LiveStream live { sources, metrics };
    std::array<Heartbeat, 2> heartbeats {};
    std::array<Ended, 2> ended {};
    std::array<bool, 2> started {};
    std::array<std::unique_ptr<RecordingSink>, 2> sinks {};
    std::array<ReactorThread, 2> reactors {}; ///< Last: stopped and joined before anything above goes.
};

} // namespace

TEST_CASE("One live stream on two real reactors: a parked push stalls neither the other subscriber nor its reactor",
          "[livestats][reactor]")
{
    // The daemon's `--threads 2` shape, with one subscriber reading slowly: each of its snapshot pushes stays parked
    // for three floors. WHAT DISTINGUISHES: the other subscriber, on the other reactor, keeps the cadence -- a
    // snapshot every tick, consecutive, no gap -- and neither reactor's thread stops running for as long as a floor,
    // while the slow one IS observed missing ticks, so the stall this case is about did happen. A stream holding
    // its lock across a push blocks the second reactor's thread on the first one's slow reader, and every clause
    // about the second subscriber goes red. Captures are shared: one per tick however many reactors ask.
    constexpr auto Park = 3 * Floor;
    TwoReactorRig rig { 0ms, Park };
    rig.Start();

    // Long enough for the slow subscriber to miss ticks at least twice, and for the fast one's cadence to mean
    // something; bounded, and the reason for a timeout is in what the checks below then find.
    auto const [waited, stalledTwice] = WaitFor(20s, [&rig] {
        return CountOf(rig.sinks[0]->Frames(), Wire::PushKind::Gap) >= 2
               && CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot) >= 12;
    });
    INFO("waited " << waited.count() << " ms");
    REQUIRE(stalledTwice);

    // Stop, and read what both saw only once every stream has returned, so nothing is still pushing.
    rig.stopping.store(true, std::memory_order_release);
    REQUIRE(rig.StreamsReturned(10s));

    auto const slow = DecodeAll(rig.sinks[0]->Frames());
    auto const fast = DecodeAll(rig.sinks[1]->Frames());
    REQUIRE_FALSE(slow.empty());
    REQUIRE_FALSE(fast.empty());
    CHECK(slow.front().kind == Wire::PushKind::Subscribed);
    CHECK(fast.front().kind == Wire::PushKind::Subscribed);

    // The fast subscriber: a snapshot on every tick, in order, none missed.
    auto const fastTicks = TicksOf(fast);
    INFO("fast subscriber's ticks " << fastTicks.size() << ", gaps " << GapsOf(fast));
    CHECK(GapsOf(fast) == 0);
    for (auto const index: std::views::iota(std::size_t { 1 }, fastTicks.size()))
    {
        INFO("tick " << index);
        CHECK(fastTicks[index] == fastTicks[index - 1] + 1);
    }

    // Both subscribed at the same moment, so their first snapshots are captures of the same tick or of adjacent
    // ones. Not EQUAL: a snapshot is labelled with the tick it was CAPTURED on, and the two reactors read the clock a
    // moment apart, so a start straddling a tick boundary hands one of them the next tick's capture. That captures
    // are shared is the census below.
    auto const slowTicks = TicksOf(slow);
    REQUIRE_FALSE(slowTicks.empty());
    INFO("first ticks: slow " << slowTicks.front() << ", fast " << fastTicks.front());
    CHECK(std::max(slowTicks.front(), fastTicks.front()) - std::min(slowTicks.front(), fastTicks.front()) <= 1U);

    // Neither reactor's thread stopped running for as long as a floor. The slow subscriber's reactor included: a
    // parked push is a parked coroutine, not a blocked thread.
    for (auto const index: { 0U, 1U })
    {
        auto const worst = std::chrono::microseconds { rig.heartbeats[index].worstMicros.load() };
        INFO("reactor " << index << " worst heartbeat lateness " << worst.count() << " us over "
                        << rig.heartbeats[index].beats.load() << " beats");
        CHECK(rig.heartbeats[index].beats.load() > 0U);
        CHECK(worst < Floor);
    }

    // Never two captures of one tick, however many reactors asked for it: no more captures than ticks either
    // subscriber was handed, and every capture rendered once.
    auto ticks = std::set<std::uint64_t> { fastTicks.begin(), fastTicks.end() };
    ticks.insert(slowTicks.begin(), slowTicks.end());
    CHECK(rig.sources.Captures() <= ticks.size());
    CHECK(rig.metrics.Read(IMetricsSink::Counter::LiveSnapshotsRendered) == rig.sources.Captures());

    // The surface is the one writer: every push reached each sink on its own reactor's thread.
    CHECK(rig.sinks[0]->OffReactor() == 0U);
    CHECK(rig.sinks[1]->OffReactor() == 0U);
}

TEST_CASE("One live stream on two real reactors: a capture running on one reactor never stops the other's thread",
          "[livestats][reactor]")
{
    // #1399 D16. WHAT DISTINGUISHES: reactor 0's subscriber is inside a capture that takes a floor and a half when
    // reactor 1's subscriber arrives, and captures are slow ONLY on reactor 0's thread -- so any time reactor 1's
    // thread stops is time spent waiting on reactor 0's capture, never on one of its own. Reactor 1 must go on
    // running: it is late by less than half a floor, and its subscriber still gets snapshots that decode.
    //
    // RED on a stream that holds one lock across the capture (`LiveStream::Observe` at `e0bd37dd`): reactor 1's
    // first `Observe` blocks its thread for the rest of reactor 0's capture. Green once a subscriber that loses the
    // tick goes on without waiting on the thread that won it.
    constexpr auto CaptureTakes = Floor + (Floor / 2);
    TwoReactorRig rig { CaptureTakes, 0ms };
    rig.StartHeartbeats();
    auto const [beating, bothBeat] =
        WaitFor(10s, [&rig] { return rig.heartbeats[0].beats.load() > 0U && rig.heartbeats[1].beats.load() > 0U; });
    INFO("heartbeats after " << beating.count() << " ms");
    REQUIRE(bothBeat);
    rig.sources.SlowOn(rig.heartbeats[0].thread.load());

    rig.StartSubscriber(0);
    auto const [capturing, begun] = WaitFor(10s, [&rig] { return rig.sources.Captures() >= 1U; });
    INFO("reactor 0's capture began after " << capturing.count() << " ms");
    REQUIRE(begun);

    // Measured from here: what reactor 1's thread does from the moment its subscriber arrives mid-capture.
    rig.heartbeats[1].worstMicros.store(0, std::memory_order_release);
    rig.StartSubscriber(1);
    auto const [waited, ran] =
        WaitFor(20s, [&rig] { return CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot) >= 4U; });
    INFO("waited " << waited.count() << " ms");
    REQUIRE(ran);
    rig.stopping.store(true, std::memory_order_release);
    REQUIRE(rig.StreamsReturned(10s));

    CHECK_FALSE(TicksOf(DecodeAll(rig.sinks[1]->Frames())).empty());
    auto const worst = std::chrono::microseconds { rig.heartbeats[1].worstMicros.load() };
    INFO("reactor 1 worst heartbeat lateness " << worst.count() << " us over " << rig.heartbeats[1].beats.load()
                                               << " beats");
    CHECK(worst < Floor / 2);
}
