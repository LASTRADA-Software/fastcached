// SPDX-License-Identifier: Apache-2.0
//
// One `LiveStream` served on two REAL reactors, the shape `fastcached --threads 2` runs (#1399).
//
// Every other live-stats case drives one `core::net::testing::TestLoop`, where one thread does everything and no
// subscriber can wait on another. The daemon shares ONE stream across all of its reactors -- the
// per-tick capture cache behind one mutex, the cap behind one atomic -- so what those cases cannot
// see is what one reactor's subscriber costs another's: a lock held across work on one thread is a
// reactor thread blocked on the other. These cases run each reactor on its own thread against the
// real clock, and judge PROGRESS rather than time: every reactor carries a heartbeat whose beats are
// the observation that its thread ran, and whose lateness is printed beside a verdict, never as one --
// a host that does not run a thread for a while moves a lateness exactly as the defect does.

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/StatsReadingCodec.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>
#include <FastCache/Protocol/LiveStream.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <core/async/ResumeOn.hpp>
#include <core/async/Task.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SleepUntil.hpp>
#include <core/platform/Clock.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;
using FastCache::Testing::AwaitUntil;
using FastCache::Testing::Reached;
using FastCache::Testing::ReactorWaitOptions;
using FastCache::Testing::Unwrap;
using FastCache::Testing::WaitOptions;
using FastCache::Testing::WaitOutcome;
using FastCache::Testing::WaitUntilOutcome;

namespace Wire = FastCache::CompileCacheWire;

namespace
{

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
            TakeTheDelay();
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

    /// Hold the slow thread's FIRST capture until @p released answers true, for at most @p bound; later ones are not held.
    ///
    /// **What turns "the other reactor ran while this capture was in progress" into an OBSERVATION.** A fixed delay
    /// leaves a case to infer that from how late the other reactor's heartbeat woke -- a magnitude on the real clock,
    /// which the host's scheduler moves as easily as the defect does. Held until the other reactor is SEEN to run, a
    /// capture on a correct stream ends once it has, and on a stream that blocks that reactor on this capture it can
    /// only end at @p bound, which `HoldExpired` then reports. Only the first: holding every capture would make one
    /// blocked reactor cost a whole bound per TICK, and a stream with the defect would then starve the case into a
    /// timeout that names a symptom rather than reaching the check that names the cause. Set before the slow
    /// thread's first capture: it is read there without a lock, after the post that starts that capture.
    /// @param released Asked on the slow thread until it answers true.
    /// @param bound The longest a capture is held.
    void HoldSlowUntil(std::function<bool()> released, std::chrono::milliseconds bound)
    {
        _released = std::move(released);
        _holdBound = bound;
    }

    /// @return Whether a capture is being held right now.
    [[nodiscard]] bool Holding() const noexcept
    {
        return _holding.load(std::memory_order_acquire);
    }

    /// @return How many captures found the condition false and were held: at most one.
    [[nodiscard]] std::size_t HoldsEntered() const noexcept
    {
        return _holdsEntered.load(std::memory_order_acquire);
    }

    /// @return Whether a held capture ran out its bound rather than being released.
    [[nodiscard]] bool HoldExpired() const noexcept
    {
        return _holdExpired.load(std::memory_order_acquire);
    }

  private:
    /// The slow thread's part of a capture: the fixed delay, or held until `HoldSlowUntil`'s condition.
    void TakeTheDelay() const
    {
        if (!_released)
        {
            std::this_thread::sleep_for(_delay);
            return;
        }
        if (_holdSpent.exchange(true, std::memory_order_acq_rel) || _released())
            return;
        _holdsEntered.fetch_add(1, std::memory_order_acq_rel);
        _holding.store(true, std::memory_order_release);
        if (DrainWithin([this] { return !_released(); },
                        DrainBound { .ceiling = _holdBound, .poll = std::chrono::milliseconds { 1 } })
            != DrainResult::Drained)
            _holdExpired.store(true, std::memory_order_release);
        _holding.store(false, std::memory_order_release);
    }

    IMetricsSink const& _metrics;
    std::chrono::milliseconds _delay;
    std::atomic<std::thread::id> _slowOn {};
    mutable std::atomic<std::size_t> _captures { 0 };
    std::function<bool()> _released {};
    std::chrono::milliseconds _holdBound { 0 };
    mutable std::atomic<std::size_t> _holdsEntered { 0 };
    mutable std::atomic<bool> _holdExpired { false };
    mutable std::atomic<bool> _holdSpent { false };
    mutable std::atomic<bool> _holding { false };
};

/// A gate that admits everyone, on every tick: who is admitted is not what these cases are about.
class OpenGate final: public ILiveGate
{
  public:
    [[nodiscard]] std::optional<std::vector<std::byte>> RefuseWatcher(LiveWatcher const& /*watcher*/) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> Admit(Wire::SubscribeRequest const& /*request*/,
                                                              LiveWatcher const& /*watcher*/) const override
    {
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::vector<std::byte>> Recheck(Wire::LiveSubject /*subject*/,
                                                                LiveWatcher const& /*watcher*/) const override
    {
        return std::nullopt;
    }
};

/// What lets a held push go, and what it has seen so far: armed as the push parks, so both count from THAT moment.
struct HeldPushCondition
{
    std::function<bool()> released;        ///< True once the push may go.
    std::function<std::string()> progress; ///< What has moved since the park, in words -- the wait's account.
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
    RecordingSink(core::net::EventLoop& reactor, std::chrono::milliseconds park, std::atomic<bool> const& stopping) noexcept:
        _reactor { reactor },
        _park { park },
        _stopping { stopping }
    {
    }

    [[nodiscard]] core::async::Task<PushOutcome> Push(std::vector<std::byte> frame,
                                                      std::chrono::milliseconds /*hold*/) override
    {
        // The surface is the one writer, so every push reaches it on its connection's reactor.
        if (!_reactor.isOnWorkerThread())
            _offReactor.fetch_add(1, std::memory_order_acq_rel);
        auto const snapshot = IsSnapshot(frame);
        {
            std::scoped_lock const guard { _mutex };
            _frames.push_back(std::move(frame));
        }
        if (snapshot && _arm && !_holdSpent.exchange(true, std::memory_order_acq_rel))
        {
            // Parked on this reactor between looks, never a blocked thread -- the same shape as the timed park
            // below, and the reason this reactor's own heartbeat can be required to go on beating meanwhile.
            auto const condition = _arm();
            _holdsEntered.fetch_add(1, std::memory_order_acq_rel);
            std::ignore = _waits.Keep(co_await AwaitUntil(
                &_reactor,
                "the parked push to be let go: the other subscriber's snapshots and both reactors' beats",
                condition.released,
                condition.progress,
                ReactorWaitOptions { .context = {}, .bound = _holdBound, .rest = HoldStep }));
            _holdEnded.store(true, std::memory_order_release);
        }
        else if (snapshot && _park > 0ms)
            co_await core::net::sleepUntil(&_reactor, _reactor.clock().now() + _park);
        co_return PushOutcome::Delivered;
    }

    /// Hold this sink's FIRST snapshot push, parked on its reactor, until what @p arm returns holds, for at most
    /// @p bound; every later snapshot push is parked as the constructor said.
    ///
    /// **A stall as PROGRESS, not as lateness.** @p arm is called as the push parks, so the condition it returns can
    /// count what happens from THAT moment -- another subscriber's snapshots, a reactor's heartbeats. On a stream that
    /// is correct those arrive however loaded the host is; on one whose parked push blocks another reactor they
    /// cannot, and the hold runs out, which `HoldReleased` reports. Only the first push, so that a stream with the
    /// defect costs one bound rather than one per tick, and a case reaches the check that names it.
    /// The condition's progress is the wait's account when the hold runs out, and it is what tells the defects
    /// apart: a reactor that stopped stops its beats, a subscriber that stopped stops only its snapshots.
    /// @param arm Called once, on this reactor, as the push parks; returns what lets it go and what has moved.
    /// @param bound The longest the push is held.
    void HoldFirstSnapshot(std::function<HeldPushCondition()> arm, std::chrono::milliseconds bound)
    {
        _arm = std::move(arm);
        _holdBound = bound;
    }

    /// @return How many pushes were held: at most one.
    [[nodiscard]] std::size_t HoldsEntered() const noexcept
    {
        return _holdsEntered.load(std::memory_order_acquire);
    }

    /// @return Whether the held push has been let go, released or at its bound.
    [[nodiscard]] bool HoldEnded() const noexcept
    {
        return _holdEnded.load(std::memory_order_acquire);
    }

    /// Whether the held push was released rather than running out its bound, with the wait's own account of what
    /// it saw attached to the case's next assertion when it ran out. The case's thread, once the streams returned.
    /// @return True unless the hold ran out.
    [[nodiscard]] bool HoldReleased() const
    {
        return _waits.AllReached();
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

    /// How often a held push looks at its condition.
    static constexpr auto HoldStep = std::chrono::milliseconds { 5 };

    core::net::EventLoop& _reactor;
    std::chrono::milliseconds _park;
    std::atomic<bool> const& _stopping;
    mutable std::mutex _mutex;
    std::vector<std::vector<std::byte>> _frames;
    std::atomic<std::size_t> _offReactor { 0 };
    std::function<HeldPushCondition()> _arm {};
    std::chrono::milliseconds _holdBound { 0 };
    std::atomic<std::size_t> _holdsEntered { 0 };
    std::atomic<bool> _holdSpent { false };
    std::atomic<bool> _holdEnded { false };
    FastCache::Testing::OffThreadWaits _waits;
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
    std::atomic<core::platform::SteadyDuration::rep> lastBeat { 0 }; ///< The latest wake-up, on the reactor's clock.
    std::atomic<std::thread::id> thread {};                          ///< The reactor's thread, as the heartbeat found it.
};

/// The step a heartbeat sleeps.
constexpr auto HeartbeatStep = 20ms;

/// How long a heartbeat may go without a beat before its reactor reads as BLOCKED rather than slow: twenty-five
/// steps, and under `Testing::ReadingWindow`, so a reactor that stops is named as stopped well inside the window a
/// wait's account reads movement over.
constexpr auto SilentAfter = 25 * HeartbeatStep;

/// When @p heartbeat last beat, on its reactor's clock.
/// @param heartbeat The heartbeat.
/// @return The instant, or nothing if it has not beaten yet.
[[nodiscard]] std::optional<core::platform::SteadyTimePoint> LastBeatAt(Heartbeat const& heartbeat)
{
    if (heartbeat.beats.load(std::memory_order_acquire) == 0U)
        return std::nullopt;
    return core::platform::SteadyTimePoint { core::platform::SteadyDuration {
        heartbeat.lastBeat.load(std::memory_order_acquire) } };
}

/// Beat on @p reactor until @p stopping.
/// @param reactor Where to beat.
/// @param heartbeat What to record into.
/// @param stopping When to end.
core::async::DetachedTask Beat(core::net::EventLoop* reactor, Heartbeat* heartbeat, std::atomic<bool> const* stopping)
{
    co_await core::async::ResumeOn { *reactor };
    heartbeat->thread.store(std::this_thread::get_id(), std::memory_order_release);
    while (!stopping->load(std::memory_order_acquire))
    {
        auto const deadline = reactor->clock().now() + HeartbeatStep;
        co_await core::net::sleepUntil(reactor, deadline);
        auto const late = std::chrono::duration_cast<std::chrono::microseconds>(reactor->clock().now() - deadline).count();
        auto worst = heartbeat->worstMicros.load(std::memory_order_acquire);
        while (late > worst && !heartbeat->worstMicros.compare_exchange_weak(worst, late))
        {
        }
        // Before the count, so a beat that has been counted has its instant.
        heartbeat->lastBeat.store(reactor->clock().now().time_since_epoch().count(), std::memory_order_release);
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
/// @param onArrival Run on @p reactor as the subscriber arrives -- in the resumption that goes on into `Serve`, whose
///        first `Observe` comes before anything in it suspends, so nothing else on @p reactor runs between the two;
///        may be empty.
core::async::DetachedTask Subscribe(LiveStream* live,
                                    core::net::EventLoop* reactor,
                                    RecordingSink* sink,
                                    OpenGate const* gate,
                                    Ended* ended,
                                    std::function<void()> onArrival)
{
    co_await core::async::ResumeOn { *reactor };
    if (onArrival)
        onArrival();
    auto const frame = Wire::EncodeSubscribeRequest(
        Wire::SubscribeRequest { .subject = Wire::LiveSubject::Cache, .cadenceMillis = 0, .dashboardToken = {} });
    (void) co_await live->Serve(frame, LiveWatcher { .host = "127.0.0.1" }, sink, gate, reactor);
    ended->returned.store(true, std::memory_order_release);
}

/// A reactor on its own thread, stopped and joined before it is destroyed on every way out of a case.
struct ReactorThread
{
    core::platform::SteadyClock clock;
    core::net::PlatformLoop reactor { clock };
    std::thread worker { [this] { reactor.run(); } };

    ReactorThread() = default;
    ReactorThread(ReactorThread const&) = delete;
    ReactorThread(ReactorThread&&) = delete;
    ReactorThread& operator=(ReactorThread const&) = delete;
    ReactorThread& operator=(ReactorThread&&) = delete;

    ~ReactorThread()
    {
        // Joined before `reactor` goes, which member order alone would not give: this body runs
        // first. Anything still parked on it is freed with it, and owns what that touches.
        reactor.stop();
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

/// Wait on the real clock until @p predicate holds or @p bound passes, through the tree's one test wait.
///
/// Polled every 10 ms rather than at the helper's default rest: a busier poll takes CPU from the reactor
/// threads whose lateness these cases measure.
/// @param what      What the case waits for, in words.
/// @param bound     How long to wait.
/// @param predicate True once it has happened.
/// @param state     What the reactors have done so far, in words.
/// @return What the wait found, and how long it took.
template <std::predicate Predicate, typename State>
[[nodiscard]] WaitOutcome WaitFor(std::string_view what, std::chrono::milliseconds bound, Predicate predicate, State state)
{
    return WaitUntilOutcome(what,
                            std::move(predicate),
                            std::move(state),
                            WaitOptions { .step = {}, .context = {}, .bound = bound, .rest = 10ms });
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
    /// @param onArrival Run on that reactor as the subscriber arrives, just before its first `Observe`; may be empty.
    void StartSubscriber(std::size_t index, std::function<void()> onArrival = {})
    {
        started.at(index) = true;
        Subscribe(&live, &reactors.at(index).reactor, sinks.at(index).get(), &gate, &ended.at(index), std::move(onArrival));
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
        return Reached(WaitFor("every subscriber started to return", bound, all, [this] {
            return std::format("returned: {}, {}", ended.at(0).returned.load(), ended.at(1).returned.load());
        }));
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
    // The daemon's `--threads 2` shape, with one subscriber reading slowly: its first snapshot push stays parked.
    // WHAT DISTINGUISHES: while it is parked, the other subscriber, on the other reactor, goes on receiving
    // snapshots, and BOTH reactors go on running -- the slow subscriber's own included, since a parked push is a
    // parked coroutine and not a blocked thread -- while the slow one IS observed missing ticks afterwards, so the
    // stall this case is about did happen. A stream holding its lock across a push blocks the second reactor's
    // thread on the first one's slow reader. Captures are shared: one per tick however many reactors ask.
    //
    // **Progress, observed -- never lateness.** This parked every slow push for three floors and then required the
    // fast subscriber's ticks to be consecutive with no gap and each reactor's heartbeat to be late by less than a
    // floor. Both are claims about the REAL clock: a tick's number is the reactor's steady clock divided by the floor
    // (`TickOf`), so a correct stream on a reactor the host did not run for a floor skips a number, and suspending
    // reactor 1 for 700 ms turned the case red in 10 runs of 10 with nothing wrong. What "stalls neither" means is
    // that both go on MOVING: so the first slow push is held until, counted from the moment it parks, the fast
    // subscriber has had `SnapshotsDuringPark` snapshots and each reactor `BeatsDuringPark` beats -- bounded, and a
    // hold that runs out is the verdict. The grid's own numbers still judge ORDER, which the clock cannot break.
    constexpr std::size_t SnapshotsDuringPark = 3;
    constexpr std::size_t BeatsDuringPark = 3;
    constexpr auto ParkHeldAtMost = 10s;
    // Each reactor's time since its last beat, in microseconds on its own clock, as the held push last looked --
    // -1 for one that had not beaten. Declared before the rig: the look runs on its reactor.
    std::array<std::atomic<std::int64_t>, 2> quietAtLastLook {};
    TwoReactorRig rig { 0ms, 0ms };
    rig.sinks[0]->HoldFirstSnapshot(
        [&rig, &quietAtLastLook] {
            auto const parkedAt = rig.reactors[0].reactor.clock().now();
            auto const fastFrom = CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot);
            auto const beatsFrom = std::array { rig.heartbeats[0].beats.load(std::memory_order_acquire),
                                                rig.heartbeats[1].beats.load(std::memory_order_acquire) };
            auto const since = [&rig, fastFrom, beatsFrom] {
                return std::array { CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot) - fastFrom,
                                    rig.heartbeats[0].beats.load(std::memory_order_acquire) - beatsFrom[0],
                                    rig.heartbeats[1].beats.load(std::memory_order_acquire) - beatsFrom[1] };
            };
            return HeldPushCondition {
                .released =
                    [since] {
                        auto const moved = since();
                        return moved[0] >= SnapshotsDuringPark && moved[1] >= BeatsDuringPark && moved[2] >= BeatsDuringPark;
                    },
                // **A reactor reads as beating or as SILENT since a fixed instant, never as a count** (review of
                // `ac9a112d`, M7): the wait calls its state MOVING while the text changes, and a count that the
                // OTHER reactor keeps raising made a reactor blocked for 9.9 s read as "still MOVING". A stopped
                // reactor now stops changing the text, and its silence as the wait gave up is reported beside it.
                .progress =
                    [&rig, &quietAtLastLook, since, parkedAt] {
                        auto const reads = [&rig, &quietAtLastLook, parkedAt](std::size_t index) -> std::string {
                            auto const last = LastBeatAt(rig.heartbeats.at(index));
                            if (!last)
                            {
                                quietAtLastLook.at(index).store(-1, std::memory_order_release);
                                return "has not beaten";
                            }
                            auto const quiet = rig.reactors.at(index).reactor.clock().now() - *last;
                            quietAtLastLook.at(index).store(
                                std::chrono::duration_cast<std::chrono::microseconds>(quiet).count(),
                                std::memory_order_release);
                            if (quiet <= SilentAfter)
                                return "beating";
                            return std::format(
                                "SILENT since its beat at {:+} ms from the park",
                                std::chrono::duration_cast<std::chrono::milliseconds>(*last - parkedAt).count());
                        };
                        auto const moved = since();
                        return std::format("since the park: the fast subscriber's snapshots +{} (wants {}); "
                                           "reactor 0 {}, reactor 1 {}",
                                           moved[0],
                                           static_cast<std::size_t>(SnapshotsDuringPark),
                                           reads(0),
                                           reads(1));
                    },
            };
        },
        ParkHeldAtMost);
    rig.Start();

    // Bounded, and the reason for a timeout is in what the checks below then find.
    auto const stalled = WaitFor(
        "the slow subscriber's held push to be let go, and its stream to report the ticks it missed",
        20s,
        [&rig] { return rig.sinks[0]->HoldEnded() && CountOf(rig.sinks[0]->Frames(), Wire::PushKind::Gap) >= 1; },
        [&rig] {
            return std::format("hold ended {}, slow subscriber's gaps {}, fast subscriber's snapshots {}",
                               rig.sinks[0]->HoldEnded(),
                               CountOf(rig.sinks[0]->Frames(), Wire::PushKind::Gap),
                               CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot));
        });
    INFO("waited " << stalled.elapsed.count() << " ms");
    REQUIRE(Reached(stalled));

    // Stop, and read what both saw only once every stream has returned, so nothing is still pushing.
    rig.stopping.store(true, std::memory_order_release);
    REQUIRE(rig.StreamsReturned(10s));

    // THE VERDICT, first once the streams have returned: the slow push was parked, and while it was, the fast
    // subscriber and both reactors went on moving.
    INFO(std::format("worst heartbeat lateness, a reading and not the verdict: reactor 0 {} us over {} beats, "
                     "reactor 1 {} us over {} beats",
                     rig.heartbeats[0].worstMicros.load(),
                     rig.heartbeats[0].beats.load(),
                     rig.heartbeats[1].worstMicros.load(),
                     rig.heartbeats[1].beats.load()));
    INFO("the held push waited for " << SnapshotsDuringPark << " fast snapshots and " << BeatsDuringPark
                                     << " beats of each reactor, for at most " << ParkHeldAtMost.count() << " s");
    INFO(std::format("time since each reactor's last beat, on its own clock, as the held push last looked (-1: none "
                     "yet): reactor 0 {} us, reactor 1 {} us",
                     quietAtLastLook[0].load(std::memory_order_acquire),
                     quietAtLastLook[1].load(std::memory_order_acquire)));
    CHECK(rig.sinks[0]->HoldsEntered() == 1U);
    CHECK(rig.sinks[0]->HoldReleased());

    auto const slow = DecodeAll(rig.sinks[0]->Frames());
    auto const fast = DecodeAll(rig.sinks[1]->Frames());
    REQUIRE_FALSE(slow.empty());
    REQUIRE_FALSE(fast.empty());
    CHECK(slow.front().kind == Wire::PushKind::Subscribed);
    CHECK(fast.front().kind == Wire::PushKind::Subscribed);

    // Order, by the grid's own numbers: each subscriber's snapshots are of strictly later ticks, never repeated and
    // never going back -- which no scheduling of a correct stream can break. Whether those numbers are CONSECUTIVE
    // is a claim about the clock the grid is cut from, and is deliberately not asserted.
    auto const fastTicks = TicksOf(fast);
    auto const slowTicks = TicksOf(slow);
    REQUIRE_FALSE(fastTicks.empty());
    REQUIRE_FALSE(slowTicks.empty());
    INFO("fast subscriber's ticks " << fastTicks.size() << ", slow subscriber's ticks " << slowTicks.size());
    CHECK(std::ranges::adjacent_find(fastTicks, std::ranges::greater_equal {}) == fastTicks.end());
    CHECK(std::ranges::adjacent_find(slowTicks, std::ranges::greater_equal {}) == slowTicks.end());

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
    // #1399 D16. WHAT DISTINGUISHES: reactor 0's subscriber is inside a capture when reactor 1's subscriber arrives,
    // and captures are slow ONLY on reactor 0's thread -- so any time reactor 1's thread stops is time spent waiting
    // on reactor 0's capture, never on one of its own. Reactor 1 must go on running, and its subscriber still gets
    // snapshots that decode.
    //
    // **Observed, not measured.** This held the capture a floor and a half and then required reactor 1's heartbeat
    // to have been late by less than half a floor -- a magnitude on the real clock, so a host that did not run
    // reactor 1 for 250 ms read as the defect with nothing wrong: suspending reactor 1 for 300 ms turned it red in 10
    // runs of 10. Real-clock lateness was never the property; reactor 1 RUNNING while reactor 0 captures is. So
    // reactor 0's capture is now HELD until reactor 1 has beaten `BeatsDuringCapture` times since its subscriber
    // arrived: a correct stream lets that happen however loaded the host is, and a stream that blocks reactor 1 on
    // this capture cannot, so the hold runs out its bound and says so. The arrival is marked ON reactor 1, in the
    // resumption that goes on into that subscriber's first `Observe` with nothing suspending between -- so every beat
    // counted comes after that `Observe`, one would do, and three is the margin. Marked on the case's thread instead,
    // before the post, a case preempted for three beats released the hold before the subscriber arrived and passed on
    // the defect: 10 of 10 with a 100 ms pause there (review of `d3bbc085`, I2). Lateness is still printed, as a
    // reading beside the verdict and never as it.
    //
    // RED on a stream that holds one lock across the capture (`LiveStream::Observe` at `e0bd37dd`): reactor 1's
    // first `Observe` blocks its thread for the rest of reactor 0's capture. Green once a subscriber that loses the
    // tick goes on without waiting on the thread that won it.
    constexpr std::size_t BeatsDuringCapture = 3;
    constexpr auto CaptureHeldAtMost = 10s;
    constexpr auto NotYetArrived = std::numeric_limits<std::size_t>::max();
    // Declared before the rig: the condition the rig holds reads them on reactor 0's thread, and the arrival writes
    // them on reactor 1's, until the rig is gone.
    std::atomic<std::size_t> beatsAtArrival { NotYetArrived };
    std::atomic<bool> holdingAtArrival { false };
    TwoReactorRig rig { 0ms, 0ms };
    rig.StartHeartbeats();
    auto const bothBeat = WaitFor(
        "both reactors' heartbeats to beat",
        10s,
        [&rig] { return rig.heartbeats[0].beats.load() > 0U && rig.heartbeats[1].beats.load() > 0U; },
        [&rig] { return std::format("beats {}, {}", rig.heartbeats[0].beats.load(), rig.heartbeats[1].beats.load()); });
    INFO("heartbeats after " << bothBeat.elapsed.count() << " ms");
    REQUIRE(Reached(bothBeat));
    rig.sources.SlowOn(rig.heartbeats[0].thread.load());
    rig.sources.HoldSlowUntil(
        [&rig, &beatsAtArrival] {
            auto const from = beatsAtArrival.load(std::memory_order_acquire);
            return from != NotYetArrived
                   && rig.heartbeats[1].beats.load(std::memory_order_acquire) >= from + BeatsDuringCapture;
        },
        CaptureHeldAtMost);

    rig.StartSubscriber(0);
    auto const begun = WaitFor(
        "reactor 0's capture to be held",
        10s,
        [&rig] { return rig.sources.Holding(); },
        [&rig] { return std::format("captures {}, held {}", rig.sources.Captures(), rig.sources.HoldsEntered()); });
    INFO("reactor 0's capture was held after " << begun.elapsed.count() << " ms");
    REQUIRE(Reached(begun));

    // From here: what reactor 1's thread does from the moment its subscriber arrives mid-capture. Whether the capture
    // was still held is read first, then the mark set -- the mark is what can let the hold go.
    rig.heartbeats[1].worstMicros.store(0, std::memory_order_release);
    rig.StartSubscriber(1, [&rig, &beatsAtArrival, &holdingAtArrival] {
        holdingAtArrival.store(rig.sources.Holding(), std::memory_order_release);
        beatsAtArrival.store(rig.heartbeats[1].beats.load(std::memory_order_acquire), std::memory_order_release);
    });
    auto const ran = WaitFor(
        "reactor 1's subscriber to see four snapshots",
        20s,
        [&rig] { return CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot) >= 4U; },
        [&rig] { return std::format("snapshots {}", CountOf(rig.sinks[1]->Frames(), Wire::PushKind::Snapshot)); });
    INFO("waited " << ran.elapsed.count() << " ms");
    REQUIRE(Reached(ran));
    rig.stopping.store(true, std::memory_order_release);
    REQUIRE(rig.StreamsReturned(10s));

    // THE VERDICT, first once the streams have returned. The capture was still held when reactor 1's subscriber
    // arrived -- read on reactor 1, at the arrival -- or the verdict below says nothing; and it was let go by reactor
    // 1 running, not by its bound.
    auto const worst = std::chrono::microseconds { rig.heartbeats[1].worstMicros.load() };
    INFO("reactor 1 worst heartbeat lateness " << worst.count() << " us over " << rig.heartbeats[1].beats.load()
                                               << " beats -- a reading, not the verdict");
    INFO("reactor 1 had beaten " << beatsAtArrival.load() << " times when its subscriber arrived; the capture waited for "
                                 << BeatsDuringCapture << " more, for at most " << CaptureHeldAtMost.count() << " s");
    CHECK(rig.sources.HoldsEntered() == 1U);
    CHECK(holdingAtArrival.load(std::memory_order_acquire));
    CHECK_FALSE(rig.sources.HoldExpired());
    CHECK_FALSE(TicksOf(DecodeAll(rig.sinks[1]->Frames())).empty());
}
