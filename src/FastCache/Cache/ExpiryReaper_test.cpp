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
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Cache/ExpiryReaper.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cache/NotifyingStorage.hpp>
#include <FastCache/Cache/ReclaimLog.hpp>
#include <FastCache/Cache/StorageTestUtils.hpp>
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace std::chrono_literals;
using FastCache::Testing::MakeBytes;
using FastCache::Testing::Unwrap;

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
        // **Called from INSIDE the sweep**, which is what makes this the cheapest
        // place to see which thread the sweep ran on (#946) -- no `IStorage` double
        // is needed, and a double would have to reimplement two dozen verbs to say
        // one thing. Blocking here blocks the sweep for the same reason.
        sweptOn.store(std::this_thread::get_id(), std::memory_order_release);
        entered.store(true, std::memory_order_release);
        events.emplace_back(std::format("{}:{}", static_cast<int>(kind), key));
    }

    std::atomic<bool> entered { false };     ///< The sweep reached this callback.
    std::atomic<std::thread::id> sweptOn {}; ///< Which thread it reached it on.

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
    // Declared in the order clang-tidy's padding check asks for rather than the
    // order the wiring reads in: `InMemoryLruStorage` puts its read counters on
    // separate cache lines, so a 64-aligned member anywhere but the front costs
    // this fixture 72 bytes of padding and fails the build. The construction
    // dependencies still hold -- `observer` precedes `log` and `storage`, `lru`
    // precedes `storage`, `clock` precedes `reactor`.
    InMemoryLruStorage lru;
    NullLogger logger;
    CancellationSource source;
    RecordingObserver observer;
    NotifyingStorage storage { lru, &observer };
    ManualClock clock;
    ReclaimLog log { &observer };
    TestReactor reactor { clock };
    AtomicMetricsSink metrics;

    Fixture()
    {
        storage.SetReclaimLog(&log);
    }
};

/// A gate one thread parks at until another opens it, with a BOUNDED wait on both
/// sides so a case that goes wrong ends red rather than hung.
class ParkGate
{
  public:
    /// Block the caller until `Open()`, recording that it arrived.
    void ParkHere()
    {
        std::unique_lock lock { _mutex };
        _parked = true;
        _changed.notify_all();
        _changed.wait_for(lock, std::chrono::seconds { 30 }, [this] { return _open; });
    }

    /// @return True once a thread is parked here, waiting at most @p bound.
    [[nodiscard]] bool WaitUntilParked(std::chrono::milliseconds bound)
    {
        std::unique_lock lock { _mutex };
        return _changed.wait_for(lock, bound, [this] { return _parked; });
    }

    /// Let the parked thread continue.
    void Open()
    {
        std::scoped_lock const lock { _mutex };
        _open = true;
        _changed.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _parked { false };
    bool _open { false };
};

/// A reactor that forwards to a `TestReactor` and can PARK the thread handing it work.
///
/// **This is what makes #1397's window deterministic.** The sweep's hop back to the reactor
/// is `IExecutor::Submit(ParkedWork)` called on the POOL thread, and the defect is that the
/// frame's owner could destroy it while that call was still in progress. Parking inside the
/// call holds the pool thread exactly there for as long as the case needs, instead of
/// waiting for a scheduler to produce the interleaving.
///
/// Parks either BEFORE forwarding (the frame is on neither side) or AFTER it (the frame is
/// already queued on the inner reactor while the pool thread has not returned).
class ParkingReactor final: public IReactor
{
  public:
    /// Where `Submit(ParkedWork)` parks. Private to this fixture: never stored or sent.
    enum class Park : std::uint8_t
    {
        No,
        BeforeForwarding,
        AfterForwarding,
    };

    explicit ParkingReactor(TestReactor& inner) noexcept:
        _inner { inner }
    {
    }

    /// Park the next `Submit(ParkedWork)` at @p where.
    void ParkNextSubmit(Park where) noexcept
    {
        _park.store(where, std::memory_order_release);
    }

    /// Make the next `Submit(ParkedWork)` throw `std::bad_alloc` without forwarding, the way
    /// an allocating `Submit` fails.
    void RefuseNextSubmit() noexcept
    {
        _refuse.store(true, std::memory_order_release);
    }

    /// @return How many `Submit(ParkedWork)` calls threw.
    [[nodiscard]] std::size_t Refused() const noexcept
    {
        return _refused.load(std::memory_order_acquire);
    }

    /// @return How many `Submit(ParkedWork)` calls have arrived.
    [[nodiscard]] std::size_t ParkedWorkSubmits() const noexcept
    {
        return _submits.load(std::memory_order_acquire);
    }

    /// @return Where a `Park` holds the handing thread, and where the case releases it.
    [[nodiscard]] ParkGate& Gate() noexcept
    {
        return _gate;
    }

    void Stop() noexcept override
    {
        _inner.Stop();
    }
    void Submit(std::coroutine_handle<> handle) override
    {
        _inner.Submit(handle);
    }
    void Submit(ParkedWork work) override
    {
        _submits.fetch_add(1, std::memory_order_acq_rel);
        if (_refuse.exchange(false, std::memory_order_acq_rel))
        {
            _refused.fetch_add(1, std::memory_order_acq_rel);
            throw std::bad_alloc {};
        }
        auto const where = _park.exchange(Park::No, std::memory_order_acq_rel);
        if (where == Park::BeforeForwarding)
            _gate.ParkHere();
        _inner.Submit(work);
        if (where == Park::AfterForwarding)
            _gate.ParkHere();
    }
    void Schedule(TimePoint deadline, std::coroutine_handle<> handle) override
    {
        _inner.Schedule(deadline, handle);
    }
    void Schedule(TimePoint deadline, ParkedWork work) override
    {
        _inner.Schedule(deadline, work);
    }
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override
    {
        return _inner.CancelPending(handle);
    }
    [[nodiscard]] IClock& Clock() noexcept override
    {
        return _inner.Clock();
    }

  protected:
    void RunLoop() override
    {
        std::ignore = _inner.Drain();
    }

  private:
    TestReactor& _inner;
    ParkGate _gate;
    std::atomic<Park> _park { Park::No };
    std::atomic<bool> _refuse { false };
    std::atomic<std::size_t> _submits { 0 };
    std::atomic<std::size_t> _refused { 0 };
};

/// An executor that forwards to another but can HOLD the next piece of work it is given, or
/// REFUSE it by throwing.
///
/// The mirror of `ParkingReactor`, for the hop OUT: the reactor thread has handed the frame
/// to the pool and gone back to its loop, and the pool has not yet resumed it. Held here,
/// that interval lasts until the case says otherwise.
class HoldingExecutor final: public IExecutor
{
  public:
    explicit HoldingExecutor(IExecutor& inner) noexcept:
        _inner { inner }
    {
    }

    /// Hold the next `Submit(ParkedWork)` instead of forwarding it.
    void HoldNextSubmit() noexcept
    {
        _hold.store(true, std::memory_order_release);
    }

    /// Make the next `Submit(ParkedWork)` throw `std::bad_alloc` instead of forwarding it.
    void RefuseNextSubmit() noexcept
    {
        _refuse.store(true, std::memory_order_release);
    }

    /// @return How many `Submit(ParkedWork)` calls threw.
    [[nodiscard]] std::size_t Refused() const noexcept
    {
        return _refused.load(std::memory_order_acquire);
    }

    /// @return How many `Submit(ParkedWork)` calls were forwarded to the inner executor.
    [[nodiscard]] std::size_t Forwarded() const noexcept
    {
        return _forwarded.load(std::memory_order_acquire);
    }

    /// @return True while a piece of work is being held.
    [[nodiscard]] bool Holding() const
    {
        std::scoped_lock const lock { _mutex };
        return _held.has_value();
    }

    /// Take the held work, leaving nothing held.
    /// @return The held work, or nothing if none was held.
    [[nodiscard]] std::optional<ParkedWork> TakeHeld()
    {
        std::scoped_lock const lock { _mutex };
        return std::exchange(_held, std::nullopt);
    }

    /// Destroy the held work instead of running it -- for a case whose reaper is already gone, where
    /// resuming the frame would run it on freed memory.
    /// @return Whether anything was held.
    bool DestroyHeld()
    {
        auto const work = TakeHeld();
        if (!work.has_value())
            return false;
        Unwrap(work).resume.destroy();
        return true;
    }

    /// Hand the held work to the inner executor.
    void ForwardHeld()
    {
        if (auto work = TakeHeld(); work.has_value())
            _inner.Submit(*work);
    }

    void Submit(std::coroutine_handle<> handle) override
    {
        _inner.Submit(handle);
    }
    void Submit(ParkedWork work) override
    {
        if (_refuse.exchange(false, std::memory_order_acq_rel))
        {
            _refused.fetch_add(1, std::memory_order_acq_rel);
            throw std::bad_alloc {};
        }
        if (_hold.exchange(false, std::memory_order_acq_rel))
        {
            std::scoped_lock const lock { _mutex };
            _held = work;
            return;
        }
        _inner.Submit(work);
        _forwarded.fetch_add(1, std::memory_order_acq_rel);
    }

  private:
    IExecutor& _inner;
    std::atomic<bool> _hold { false };
    std::atomic<bool> _refuse { false };
    std::atomic<std::size_t> _refused { 0 };
    std::atomic<std::size_t> _forwarded { 0 };
    mutable std::mutex _mutex;
    std::optional<ParkedWork> _held;
};

/// @return What @p reactor has seen, in words, for a fixture wait that ran out.
[[nodiscard]] std::string Describe(ParkingReactor& reactor)
{
    return std::format("hop-back submits {} (refused {}), a thread parked at the gate {}",
                       reactor.ParkedWorkSubmits(),
                       reactor.Refused(),
                       reactor.Gate().WaitUntilParked(0ms));
}

/// @return What @p executor has seen, in words, for a fixture wait that ran out.
[[nodiscard]] std::string Describe(HoldingExecutor const& executor)
{
    return std::format(
        "hop-out work held {}, forwarded {}, refused {}", executor.Holding(), executor.Forwarded(), executor.Refused());
}

/// @return Whether @p reaper's sweep is away on its executor, in words, for a fixture wait that ran out.
/// Nothing else: `Cycles()` is not atomic, and the executor writes it.
[[nodiscard]] std::string Describe(ExpiryReaper const& reaper)
{
    return std::format("sweep away on the executor {}", reaper.AwayFromReactor());
}

/// An abandonment that RETURNS and counts, so a case can drive `Stop` past its drain ceiling
/// and see what it did instead of watching the process end.
///
/// **More permissive than the seam it stands for, and a case pays for that** (#1427). Production
/// ends the process here because returning lets `~ExpiryReaper` free the reaper while a frame is
/// still inside it; this returns, so the destructor completes. So a case whose sweep leaves on a
/// REAL executor thread owns its reaper through `ReaperOnceBack`, which never frees a reaper that
/// abandoned. The other two shapes are safe as they are: a frame HELD on the executor is run by
/// nothing, and a reactor that is its own executor never raises the count `Stop` waits on.
class RecordingAbandonment final: public IDrainAbandonment
{
  public:
    void Abandon() noexcept override
    {
        calls.fetch_add(1, std::memory_order_acq_rel);
    }

    std::atomic<int> calls { 0 }; ///< How many times `Stop` abandoned a frame.
};

/// How long a case gives a stop to return BEFORE it releases the park. On the defect it
/// returns at once, so the look can be short; a fixed stop does not return until the
/// release however long the look is, so a slow host cannot turn the fix red.
constexpr auto StopLook = 200ms;

/// How long another thread may take before a case calls it hung. A hang guard, not a race: a
/// thread that is merely slow gets there inside it on any host that runs this suite at all, and
/// one that never does fails at it. Its readers: `HeldCeilingDrainWait` lets a stop drain reach
/// its ceiling only past it, `StopWhileAway` waits at most twice it for a released stop to end,
/// and `TickUntil` gives every fixture wait at most it.
constexpr auto TripHangGuard = 10s;

/// A cycle that sweeps every millisecond and notices a stop within one, so a case reaches a hop in a
/// few ticks -- with a stop drain short enough for a case to wait out.
constexpr ExpiryReaperOptions FastCycleShortDrain { .interval = 1ms,
                                                    .stopWakeBound = 1ms,
                                                    .stopDrain = DrainBound { .ceiling = 20ms, .poll = 1ms } };

/// The same cycle with a stop drain ceiling of ONE millisecond, for a reaper whose drain clock the
/// case holds (`HeldCeilingDrainWait`).
///
/// **The tiny ceiling is the check that the clock is the held one** (#1433). Held, the clock never
/// moves and no ceiling is ever reached, so the case is exactly what it was. If `Stop` measured real
/// time instead -- the seam not wired -- one millisecond passes inside any case that holds a frame
/// away, the drain gives up, and the case's `Abandonments() == 0` fails on every host. Not zero:
/// `DrainWithin` tests `Now() >= deadline`, so a zero ceiling is reached by a clock that never moves.
constexpr ExpiryReaperOptions FastCycleHeldDrain { .interval = 1ms,
                                                   .stopWakeBound = 1ms,
                                                   .stopDrain = DrainBound { .ceiling = 1ms, .poll = 1ms } };
// The look must outlast the ceiling, or an unwired seam's real clock could end the look first.
static_assert(StopLook > FastCycleHeldDrain.stopDrain.ceiling);

/// A drain wait whose clock stands still, so a stop drain never reaches its ceiling while a case
/// holds a frame away -- until a REAL-time hang guard passes (#1433).
///
/// **A ceiling the case is not asserting on becomes a condition instead of a race.** `Stop` gives a
/// frame away on the executor `stopDrain.ceiling` of drain-clock time; measured on the host's clock,
/// that is a race a loaded host loses, ending the binary with status 75 or -- behind a returning
/// abandonment -- freeing a reaper under a live frame. Held, the wait ends when the frame comes
/// back, which is the fact the stop exists to wait for.
///
/// **Past `TripHangGuard` of real time the clock jumps beyond any ceiling**, so a frame that truly
/// never comes back still ends the drain -- at the ceiling, through the abandonment, as a red. The
/// guard starts at the drain's first look, not at construction, so a slow set-up does not spend it.
class HeldCeilingDrainWait final: public IDrainWait
{
  public:
    [[nodiscard]] TimePoint Now() const noexcept override
    {
        auto const real = DefaultDrainWait().Now();
        // The first look claims the slot; a later one reads it back from the failed exchange.
        auto firstRep = TimePoint::rep { 0 };
        if (_firstLook.compare_exchange_strong(firstRep, real.time_since_epoch().count(), std::memory_order_acq_rel))
            firstRep = real.time_since_epoch().count();
        auto const first = TimePoint { TimePoint::duration { firstRep } };
        // Monotonic both sides of the guard: frozen at `first`, then real time a day ahead.
        return real - first < TripHangGuard ? first : real + std::chrono::hours { 24 };
    }

    void Sleep(std::chrono::milliseconds requested) noexcept override
    {
        DefaultDrainWait().Sleep(requested);
    }

  private:
    mutable std::atomic<TimePoint::rep> _firstLook { 0 };
};

/// The drain seam a fixture wait runs on: each poll TICKS the case's reactor, and time is the
/// host's monotonic clock (#1433).
///
/// A tick advances the manual clock a millisecond first, so the cycle's own timers fire, then
/// sleeps 100 us -- longer where the host's timer is coarse -- so the thread the case waits on (the
/// pool, or a hop parking at a gate) can run. The requested poll is not a duration here: the
/// cadence is one tick.
///
/// **A tick that throws is kept, not swallowed and not fatal.** `Sleep` is `noexcept`, and an
/// exception escaping it would end the whole test binary; the wait stops at it instead and
/// `TickUntil` rethrows it to the case, where Catch2 reports it as that case's failure.
class TickingDrainWait final: public IDrainWait
{
  public:
    /// @param clock   The case's manual clock; must outlive this.
    /// @param reactor The reactor each poll ticks; must outlive this.
    TickingDrainWait(ManualClock& clock, TestReactor& reactor) noexcept:
        _clock { clock },
        _reactor { reactor }
    {
    }

    [[nodiscard]] TimePoint Now() const noexcept override
    {
        return DefaultDrainWait().Now();
    }

    void Sleep(std::chrono::milliseconds /*requested*/) noexcept override
    {
        _clock.Advance(1ms);
        try
        {
            std::ignore = _reactor.Tick();
        }
        catch (...)
        {
            _thrown = std::current_exception();
        }
        ++_ticks;
        std::this_thread::sleep_for(100us);
    }

    /// @return How many ticks the wait has run.
    [[nodiscard]] std::size_t Ticks() const noexcept
    {
        return _ticks;
    }

    /// @return What a tick threw, or null.
    [[nodiscard]] std::exception_ptr Thrown() const noexcept
    {
        return _thrown;
    }

  private:
    ManualClock& _clock;
    TestReactor& _reactor;
    std::size_t _ticks { 0 };
    std::exception_ptr _thrown;
};

/// Tick the case's reactor until @p reached holds, for at most `TripHangGuard` of REAL time.
///
/// **Bounded by time on a monotonic clock, never by a count of ticks** (#1433). What these cases
/// wait for is done by ANOTHER thread, and a count of ticks is a race a loaded host loses. Through
/// `DrainWithin`, the tree's one bounded wait, which measures.
///
/// **A wait that ran out says what it waited for and what it found**, attached to the case's next
/// assertion (`UNSCOPED_INFO`, since a scoped message would die here): the real time and ticks it
/// spent, the reactor's queues, @p state at the end, and how long @p state had been quiet -- read
/// as MOVING, STALLED, or INCONCLUSIVE where the numbers cannot separate those.
/// @param f       The case's clock and reactor.
/// @param what    What the case waits for, in words.
/// @param reached True once it has happened.
/// @param state   What the threads involved have done so far, in words.
/// @return Whether @p reached held within the guard.
template <typename Predicate, typename State>
[[nodiscard]] bool TickUntil(Fixture& f, std::string_view what, Predicate reached, State state)
{
    TickingDrainWait ticking { f.clock, f.reactor };
    // The account is timed on the host's clock directly, never through the seam the wait ran on: a
    // seam that counted instead of measuring then shows as a wait too short to read.
    auto const started = DefaultDrainWait().Now();
    auto seen = state();
    auto lastChange = started;
    auto changes = 0;
    auto const busy = [&] {
        if (ticking.Thrown() != nullptr || reached())
            return false;
        if (auto now = state(); now != seen)
        {
            seen = std::move(now);
            lastChange = DefaultDrainWait().Now();
            ++changes;
        }
        return true;
    };
    auto const result = DrainWithin(busy, DrainBound { .ceiling = TripHangGuard, .poll = 1ms }, ticking);
    if (auto const thrown = ticking.Thrown(); thrown != nullptr)
        std::rethrow_exception(thrown);
    if (result == DrainResult::Drained)
        return true;
    auto const ended = DefaultDrainWait().Now();
    auto const waited = ended - started;
    auto const quiet = ended - lastChange;
    auto const ms = [](auto span) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(span).count();
    };
    // Four readings, because two would each claim the cases between them: a wait shorter than the
    // window cannot tell a stall from a slow thread at all, and a state that moved and then went
    // quiet for part of the wait is what a backed-off cycle and a stuck thread BOTH look like.
    constexpr auto Window = 1s;
    auto const reading =
        waited < Window   ? "INCONCLUSIVE: too short a wait to tell a stall from a slow thread"
        : quiet <= Window ? "still MOVING at the guard: slow, or spinning"
        : quiet * 2 > waited
            ? "STALLED: nothing it reports moved for most of the wait"
            : "INCONCLUSIVE: it moved, then went quiet -- a backed-off cycle and a stuck thread both read so";
    UNSCOPED_INFO(std::format("TickUntil gave up waiting for {} after {} ms of real time and {} ticks; the reactor "
                              "holds {} submission(s) and {} timer(s). State at the end: {}. It changed {} time(s), "
                              "and nothing changed in the last {} ms: {}.",
                              what,
                              ms(waited),
                              ticking.Ticks(),
                              f.reactor.PendingSubmissions(),
                              f.reactor.PendingTimers(),
                              seen,
                              changes,
                              ms(quiet),
                              reading));
    return false;
}

/// Owns a reaper whose sweep leaves on a real executor thread: its stop drain runs on a held clock,
/// and a reaper that abandoned is never freed (#1427, #1433).
///
/// **The drain is a CONDITION the case controls, not a ceiling it races.** The reaper is built over
/// `HeldCeilingDrainWait`, so `Stop`'s OWN wait -- the behaviour several of these cases assert --
/// ends when the frame comes back, however slow the host. It is not bypassed: an owner that waited
/// on the frame itself before calling `Stop` (#1427's first shape) left `Stop`'s wait unexercised.
///
/// **The owner, not a call beside it**, because the destructor is the one path every exit takes:
/// a `REQUIRE` failing above an explicit stop unwinds straight into `~unique_ptr`, which frees the
/// reaper after whatever `Stop` did.
///
/// **A reaper that abandoned is RELEASED, not destroyed** -- leaked on purpose. The abandonment
/// returns here where production's ends the process, so `Stop` has left a frame another thread may
/// still be inside; freeing the reaper under it is a use-after-free, and a hung trip must be a RED,
/// never a crash. The seam cannot provide this by itself: once `Abandon()` returns, `~unique_ptr`
/// would free the reaper regardless.
class ReaperOnceBack
{
  public:
    /// Over `FastCycleHeldDrain`, always: its one-millisecond ceiling is the check that the held
    /// clock is the one `Stop` reads, so the clock and the ceiling are chosen in this one place.
    /// @param storage What the reaper sweeps; must outlive this.
    /// @param logger  Where it logs; must outlive this.
    ReaperOnceBack(IStorage& storage, ILogger& logger):
        _reaper { std::make_unique<ExpiryReaper>(storage, logger, FastCycleHeldDrain, nullptr, _abandonment, _drainWait) }
    {
    }

    ReaperOnceBack(ReaperOnceBack const&) = delete;
    ReaperOnceBack(ReaperOnceBack&&) = delete;
    ReaperOnceBack& operator=(ReaperOnceBack const&) = delete;
    ReaperOnceBack& operator=(ReaperOnceBack&&) = delete;

    ~ReaperOnceBack()
    {
        std::ignore = Stop();
    }

    /// @return The owned reaper; valid until `Stop`.
    [[nodiscard]] ExpiryReaper* operator->() const noexcept
    {
        return _reaper.get();
    }

    /// @return The owned reaper; valid until `Stop`.
    [[nodiscard]] ExpiryReaper& operator*() const noexcept
    {
        return *_reaper;
    }

    /// @return How many times the reaper's `Stop` abandoned a frame. Zero unless a trip never came back.
    [[nodiscard]] int Abandonments() const noexcept
    {
        return _abandonment.calls.load(std::memory_order_acquire);
    }

    /// Stop the reaper through its own drain, then destroy it -- or, if it abandoned, leak it.
    /// Idempotent: a second call finds nothing to stop.
    /// @return Whether the stop abandoned nothing and the reaper was destroyed.
    [[nodiscard]] bool Stop()
    {
        if (_reaper == nullptr)
            return Abandonments() == 0;
        _reaper->Stop();
        if (Abandonments() != 0)
        {
            std::ignore = _reaper.release();
            return false;
        }
        _reaper.reset();
        return true;
    }

  private:
    // Declared BEFORE the reaper, which borrows both, and so destroyed after it -- or outlived by a
    // reaper this leaked, which touches neither again: only `Stop` reads them, and it has run.
    RecordingAbandonment _abandonment;
    HeldCeilingDrainWait _drainWait;
    std::unique_ptr<ExpiryReaper> _reaper;
};

/// What a staged stop observed.
struct StagedStop
{
    bool returnedBeforeRelease; ///< The stop returned while the frame was still away.
    bool finished;              ///< The stop returned once the frame was released.
};

/// Stop @p reaper on a thread of its own while the case holds its frame away, then release.
///
/// A thread of its own because a fixed `Stop` WAITS for the frame to come back, and the release
/// that lets it come back is this thread's. The look before the release is the assertion;
/// `std::async`'s future joins that thread on every path out. A stop that gave up is caught by the
/// owner, which leaks the reaper rather than freeing it, so the release below cannot run the frame
/// into freed memory.
/// @param reaper  The reaper to stop.
/// @param staged  Whether the case reached the window; if not, nothing is stopped here.
/// @param release Lets the held frame continue. Called whether or not @p staged.
/// @return What the stop did either side of the release.
template <typename Release>
[[nodiscard]] StagedStop StopWhileAway(ReaperOnceBack& reaper, bool staged, Release release)
{
    if (!staged)
    {
        release();
        return { .returnedBeforeRelease = false, .finished = true };
    }
    auto stopped = std::async(std::launch::async, [&reaper] { std::ignore = reaper.Stop(); });
    auto const returnedBeforeRelease = stopped.wait_for(StopLook) == std::future_status::ready;
    release();
    // Twice the guard: a trip that never returns ends the held drain at `TripHangGuard`, and that
    // must be what this reports rather than a look that gave up first.
    return { .returnedBeforeRelease = returnedBeforeRelease,
             .finished = stopped.wait_for(2 * TripHangGuard) == std::future_status::ready };
}

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
    auto task = reaper.Run(&f.reactor, &f.reactor, f.source.Token());
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
    auto task = reaper.Run(&f.reactor, &f.reactor, f.source.Token());
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
        reaper.Start(f.reactor, f.reactor);
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
    auto task = reaper.Run(&f.reactor, &f.reactor, f.source.Token());
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
    auto task = reaper.Run(&f.reactor, &f.reactor, f.source.Token());
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

TEST_CASE("The scan budget is adapted from measured sweep cost, not trusted", "[expiry][reaper]")
{
    // #946. `scanBudget` counts ENTRIES, and entries are not work: `DefaultScanBudget`'s
    // own note calls 512 "small enough that no single cycle is measurable against a
    // request", which is an assumption about per-entry cost. That cost varies by three
    // orders of magnitude between a tier of small values and one of multi-megabyte
    // objects, so a ceiling in entries bounds how many keys a sweep touches and says
    // nothing about how long the reactor is unavailable -- which is the thing protected.
    //
    // The rule is tested where it lives. Driving it through `Run` would need an
    // `IStorage` fake that consumes clock time: 28 forwarding methods to observe one
    // behaviour. What that leaves unasserted is the wiring, and the case below on a
    // real cycle is what covers the call actually happening.
    InMemoryLruStorage lru;
    NullLogger logger;
    ExpiryReaperOptions options;
    options.scanBudget = 512;
    options.sweepStallCeiling = std::chrono::milliseconds { 50 };
    ExpiryReaper reaper { lru, logger, options, nullptr };

    REQUIRE(reaper.CurrentScanBudget() == 512);

    SECTION("an overrun halves it, at once")
    {
        // Halve down and step up are deliberately asymmetric: an overrun is a stall an
        // operator can feel, while being under the ceiling is only slower reclamation.
        reaper.AdaptScanBudget(std::chrono::milliseconds { 200 });
        CHECK(reaper.CurrentScanBudget() == 256);
        reaper.AdaptScanBudget(std::chrono::milliseconds { 200 });
        CHECK(reaper.CurrentScanBudget() == 128);
    }

    SECTION("it never decays below the floor, because zero means NO ceiling")
    {
        // `PurgeBudget` spells "no ceiling" as 0, so a budget that decayed to it would
        // become an unbounded scan -- the exact opposite of this mechanism's purpose.
        for (int i = 0; i < 40; ++i)
            reaper.AdaptScanBudget(std::chrono::seconds { 5 });
        CHECK(reaper.CurrentScanBudget() >= 8);
        CHECK(reaper.CurrentScanBudget() > 0);
    }

    SECTION("headroom grows it back, and never past what the operator configured")
    {
        reaper.AdaptScanBudget(std::chrono::seconds { 5 }); // 256
        REQUIRE(reaper.CurrentScanBudget() == 256);
        for (int i = 0; i < 50; ++i)
            reaper.AdaptScanBudget(std::chrono::milliseconds { 1 });
        // The configured value is the operator's ceiling: this only ever takes budget
        // AWAY from it, so no amount of headroom may exceed it.
        CHECK(reaper.CurrentScanBudget() == 512);
    }

    SECTION("a zero ceiling disables adaptation rather than meaning `never stall`")
    {
        ExpiryReaperOptions off;
        off.scanBudget = 512;
        off.sweepStallCeiling = Duration::zero();
        ExpiryReaper fixed { lru, logger, off, nullptr };
        fixed.AdaptScanBudget(std::chrono::seconds { 30 });
        CHECK(fixed.CurrentScanBudget() == 512);
    }
}

TEST_CASE("The sweep body runs on the executor it was given and not on the reactor", "[expiry][reaper][offreactor]")
{
    // #946's acceptance is a THREAD IDENTITY assertion rather than a behavioural
    // one, and that is the whole point: the sweep reclaims the same keys either way,
    // so a test that checks the outcome passes with the sweep still inline. The trap
    // this ticket names; `CompileResponder_test.cpp` makes the same argument for the
    // compile hop.
    //
    // Deliberately does NOT block inside the sweep. An earlier version held the sweep
    // in the observer to also show the reactor still serving, and it deadlocked the
    // fixture -- the pool thread holds the observer while the test thread pumps the
    // same reactor. The property that decides #946 is WHICH THREAD ran the sweep, and
    // that needs no blocking to see.
    Fixture f;
    REQUIRE(f.storage.Set("k", MakeBytes("v"), 0, f.clock.Now() + 1ms).has_value());
    f.clock.Advance(10ms);

    // **Reset AFTER the Set.** `NotifyingStorage::Set` notifies too, so without this
    // the observer records the store -- on the test thread -- and the case reads a
    // pass or a fail about an event that is not the sweep. It cost a red run to find,
    // and the red was the instrument working.
    f.observer.entered.store(false, std::memory_order_release);
    f.observer.sweptOn.store(std::thread::id {}, std::memory_order_release);

    ThreadPoolExecutor pool { 1 };
    auto const reactorThread = std::this_thread::get_id();
    // Owned through `ReaperOnceBack`: the sweep may still be on the pool when the case stops it,
    // and a stop measured on the host's clock would race it (#1433).
    ReaperOnceBack reaper { f.storage, f.logger };
    reaper->Start(f.reactor, pool);

    // `TickUntil` rather than `Drain()`: Drain runs until a tick advances nothing, and a
    // cycle whose interval keeps re-arming always has another timer, so Drain does not
    // return on this fixture.
    REQUIRE(TickUntil(
        f,
        "the sweep to reach the observer on the pool thread",
        [&] { return f.observer.entered.load(std::memory_order_acquire); },
        [&] { return Describe(*reaper); }));

    // **The assertion.** The sweep body did not run on the reactor's thread.
    CHECK(f.observer.sweptOn.load(std::memory_order_acquire) != reactorThread);
    CHECK(reaper.Stop());
}

TEST_CASE("Passing the reactor as the sweep executor keeps the sweep on the loop", "[expiry][reaper][offreactor]")
{
    // The control, and it is what makes the case above mean something: with the
    // reactor passed as its own executor the sweep runs on the loop, exactly as it
    // did before #946. `IReactor` IS an `IExecutor`, so this is the same statements
    // rather than a second code path -- and without this, "the sweep is elsewhere"
    // could be true because the hop always leaves, which would be a different defect.
    Fixture f;
    REQUIRE(f.storage.Set("k", MakeBytes("v"), 0, f.clock.Now() + 1ms).has_value());
    f.clock.Advance(10ms);

    // **Reset AFTER the Set.** `NotifyingStorage::Set` notifies too, so without this
    // the observer records the store -- on the test thread -- and the case reads a
    // pass or a fail about an event that is not the sweep. It cost a red run to find,
    // and the red was the instrument working.
    f.observer.entered.store(false, std::memory_order_release);
    f.observer.sweptOn.store(std::thread::id {}, std::memory_order_release);

    auto const reactorThread = std::this_thread::get_id();
    {
        ExpiryReaper reaper { f.storage, f.logger, ExpiryReaperOptions { .interval = 1ms, .stopWakeBound = 1ms } };
        reaper.Start(f.reactor, f.reactor);
        REQUIRE(TickUntil(
            f,
            "the sweep to reach the observer on the reactor",
            [&] { return f.observer.entered.load(std::memory_order_acquire); },
            // `Cycles()` is not atomic: readable here only because the sweep runs on this thread.
            [&] { return std::format("cycles {}", reaper.Cycles()); }));
        CHECK(f.observer.sweptOn.load(std::memory_order_acquire) == reactorThread);
    }
    SUCCEED("the sweep stayed on the reactor when the reactor was the executor");
}

TEST_CASE("Stopping the expiry cycle waits until the hop back has reached the reactor", "[expiry][reaper][offreactor]")
{
    // #1397, the shape the gate caught: the pool thread is INSIDE the reactor's `Submit` for
    // the hop back, the frame is on neither side, and the owner destroys it. The flag the
    // destructor waited on was cleared BEFORE the hop began, so it answered "not sweeping"
    // and `~Task` freed a frame the pool thread was still handing over.
    //
    // Stopped on another thread, because a fixed stop waits for the hop back and the hop
    // back is parked by this case. The look before the release is the assertion;
    // the tick after it is where the defect shows as ASan's heap-use-after-free -- the inner
    // reactor resumes a handle whose frame the destructor already freed.
    Fixture f;
    ThreadPoolExecutor pool { 1 };
    ParkingReactor reactor { f.reactor };
    ReaperOnceBack reaper { f.storage, f.logger };
    reactor.ParkNextSubmit(ParkingReactor::Park::BeforeForwarding);
    reaper->Start(reactor, pool);
    auto const parked = TickUntil(
        f,
        "the pool thread's hop back to park before forwarding",
        [&] { return reactor.Gate().WaitUntilParked(0ms); },
        [&] { return std::format("{}; {}", Describe(reactor), Describe(*reaper)); });

    auto const stop = StopWhileAway(reaper, parked, [&] { reactor.Gate().Open(); });

    REQUIRE(parked);
    CHECK_FALSE(stop.returnedBeforeRelease);
    CHECK(stop.finished);
    // The wait ended because the frame came back, never at the ceiling: see `FastCycleHeldDrain`.
    CHECK(reaper.Abandonments() == 0);
    // Taken back off the reactor rather than left for it: nothing may resume it now.
    CHECK(f.reactor.PendingSubmissions() == 0);
    CHECK(f.reactor.Tick() == 0);
}

TEST_CASE("Stopping the expiry cycle waits for a frame handed to its executor and not yet resumed",
          "[expiry][reaper][offreactor]")
{
    // #1397's mirror window: the reactor thread has handed the frame to the executor and
    // gone back to its loop, and the executor has not resumed it yet. Nothing marked that
    // interval -- the flag was set only once the body began -- so the destructor saw "not
    // sweeping", `CancelPending` could not find the frame, and `~Task` freed it. Held here
    // instead of raced, and the release is what the pool then resumes.
    Fixture f;
    ThreadPoolExecutor pool { 1 };
    HoldingExecutor executor { pool };
    ReaperOnceBack reaper { f.storage, f.logger };
    executor.HoldNextSubmit();
    reaper->Start(f.reactor, executor);
    auto const held = TickUntil(
        f,
        "the hop out to be held on the executor",
        [&] { return executor.Holding(); },
        [&] { return std::format("{}; {}", Describe(executor), Describe(*reaper)); });

    auto const stop = StopWhileAway(reaper, held, [&] { executor.ForwardHeld(); });

    REQUIRE(held);
    CHECK_FALSE(stop.returnedBeforeRelease);
    CHECK(stop.finished);
    CHECK(reaper.Abandonments() == 0);
    CHECK(f.reactor.PendingSubmissions() == 0);
    CHECK(f.reactor.Tick() == 0);
}

TEST_CASE("A late return from one hop back does not end the wait for the next trip off the reactor",
          "[expiry][reaper][offreactor]")
{
    // The reason the wait is on a COUNT and not a flag. The pool thread returns from the
    // reactor's `Submit` AFTER the reactor has already resumed the frame, run the loop and
    // sent it away again -- a multi-worker pool can do this with no parking at all. A flag
    // cleared on that late return would clobber the next hop's "away", and the destructor
    // would free a frame that is sitting on the executor.
    //
    // Staged: the first hop back parks AFTER forwarding, the reactor runs the frame into the
    // next hop out, which the executor holds, and only then does the late return land.
    Fixture f;
    ThreadPoolExecutor pool { 1 };
    ParkingReactor reactor { f.reactor };
    HoldingExecutor executor { pool };
    ReaperOnceBack reaper { f.storage, f.logger };
    reactor.ParkNextSubmit(ParkingReactor::Park::AfterForwarding);
    reaper->Start(reactor, executor);
    auto const state = [&] {
        return std::format("{}; {}; {}", Describe(reactor), Describe(executor), Describe(*reaper));
    };
    auto const parked = TickUntil(
        f,
        "the pool thread's hop back to park after forwarding",
        [&] { return reactor.Gate().WaitUntilParked(0ms); },
        state);

    // The pool thread is still inside the first hop back; the frame is queued on the inner
    // reactor. Run it into the SECOND hop out, and hold that one.
    executor.HoldNextSubmit();
    auto const heldAgain =
        parked && TickUntil(f, "the second hop out to be held on the executor", [&] { return executor.Holding(); }, state);

    // Now the late return from the first hop back.
    reactor.Gate().Open();

    auto const stop = StopWhileAway(reaper, heldAgain, [&] { executor.ForwardHeld(); });

    // `heldAgain` FIRST: it is false whenever either wait ran out, and `TickUntil`'s account is an
    // `UNSCOPED_INFO` the next assertion consumes even when it PASSES -- so a passing
    // `REQUIRE(parked)` ahead of it would swallow the second wait's account.
    REQUIRE(heldAgain);
    REQUIRE(parked);
    CHECK_FALSE(stop.returnedBeforeRelease);
    CHECK(stop.finished);
    CHECK(reaper.Abandonments() == 0);
    CHECK(f.reactor.PendingSubmissions() == 0);
    CHECK(f.reactor.Tick() == 0);
}

TEST_CASE("Stopping a cycle whose executor is its own reactor does not wait for the frame", "[expiry][reaper][offreactor]")
{
    // The control for the rule that the wait covers only a frame that LEFT the reactor. With
    // the reactor as its own executor the hop out lands on the reactor's own queue, which
    // `CancelPending` takes back -- so there is nothing to wait for. Counted anyway, a frame
    // parked there when the loop has already stopped would never come back to lower the
    // count, and every in-memory shutdown would wait out the drain ceiling and then end the
    // process as though a sweep were stuck on an executor.
    //
    // The ceiling is short and the abandonment is a seam that returns, so that defect shows
    // here as an assertion rather than as the test process ending with status 75.
    Fixture f;
    CapturingLogger logger;
    RecordingAbandonment abandonment;
    ParkingReactor reactor { f.reactor };
    auto reaper = std::make_unique<ExpiryReaper>(f.storage, logger, FastCycleShortDrain, nullptr, abandonment);
    reaper->Start(reactor, reactor);

    // One tick at a time until the hop out has been handed to the reactor. `Tick` swaps the
    // ready batch before resuming, so that submission waits for a NEXT tick that never runs.
    REQUIRE(TickUntil(
        f,
        "the hop out to be handed to the reactor",
        [&] { return reactor.ParkedWorkSubmits() != 0; },
        // `Cycles()` is not atomic: readable here only because the sweep runs on this thread.
        [&] { return std::format("{}; cycles {}", Describe(reactor), reaper->Cycles()); }));
    REQUIRE(reactor.ParkedWorkSubmits() == 1);
    REQUIRE(f.reactor.PendingSubmissions() == 1);

    reaper.reset();

    CHECK(abandonment.calls.load(std::memory_order_acquire) == 0);
    CHECK(f.reactor.PendingSubmissions() == 0);
    CHECK_FALSE(std::ranges::any_of(logger.Snapshot(), [](auto const& record) {
        return record.level == LogLevel::Warn || record.level == LogLevel::Error;
    }));
}

TEST_CASE("A sweep frame that never comes back is abandoned by ending the process and never freed",
          "[expiry][reaper][offreactor]")
{
    // The drain ceiling's other half. Past it there is no safe way to carry on: the frame's
    // code reaches into the reaper, so freeing it frees a coroutine another thread is inside,
    // and returning lets the reaper die underneath that thread. Production ends the process;
    // this seam returns, so the case can see the ceiling reached, the reason stated, and the
    // frame left alone.
    //
    // The frame is HELD on the executor, so it cannot come back. Destroyed below by the case,
    // which now owns it: had `~Task` already freed it, that destruction is a double free.
    Fixture f;
    CapturingLogger logger;
    RecordingAbandonment abandonment;
    ThreadPoolExecutor pool { 1 };
    HoldingExecutor executor { pool };
    auto reaper = std::make_unique<ExpiryReaper>(f.storage, logger, FastCycleShortDrain, nullptr, abandonment);
    executor.HoldNextSubmit();
    reaper->Start(f.reactor, executor);
    REQUIRE(TickUntil(
        f,
        "the hop out to be held on the executor",
        [&] { return executor.Holding(); },
        [&] { return std::format("{}; {}", Describe(executor), Describe(*reaper)); }));

    reaper.reset();

    CHECK(abandonment.calls.load(std::memory_order_acquire) == 1);
    CHECK(std::ranges::any_of(logger.Snapshot(), [](auto const& record) {
        return record.level == LogLevel::Error
               && record.message.find("ending the process rather than freeing a coroutine another thread is still inside")
                      != std::string::npos;
    }));
    // Still held, so nothing has resumed it; this is its one and only destruction.
    REQUIRE(executor.DestroyHeld());
}

TEST_CASE("A sweep that could not be handed to its executor is skipped without stranding the stop",
          "[expiry][reaper][offreactor]")
{
    // An executor's `Submit` allocates, so it can throw. The trip count is raised before the
    // hand-over, and a hand-over that threw never moved the frame: left raised, the count
    // would describe a trip nobody is on, and the stop would wait out its ceiling and end the
    // process. And the exception resumes the frame on the reactor, where ending the cycle over
    // one failed allocation would mean nothing expires again.
    Fixture f;
    CapturingLogger logger;
    ThreadPoolExecutor pool { 1 };
    HoldingExecutor executor { pool };
    ReaperOnceBack reaper { f.storage, logger };
    executor.RefuseNextSubmit();
    reaper->Start(f.reactor, executor);
    auto const state = [&] {
        return std::format("{}; {}", Describe(executor), Describe(*reaper));
    };
    REQUIRE(TickUntil(f, "the hop out to be refused", [&] { return executor.Refused() == 1; }, state));
    // The cycle survived it: a later sweep was handed over.
    REQUIRE(TickUntil(f, "a later hop out to be forwarded", [&] { return executor.Forwarded() != 0; }, state));

    // `Stop`'s own drain, on a held clock: see `ReaperOnceBack`. A count the refused hand-over left
    // raised never falls, the hang guard ends the drain at its ceiling, and it fails HERE -- which is
    // this case's verdict.
    REQUIRE(reaper.Stop());
    CHECK_FALSE(std::ranges::any_of(logger.Snapshot(), [](auto const& record) { return record.level == LogLevel::Error; }));
}

TEST_CASE("A hop back that could not be handed to the reactor still brings the sweep back", "[expiry][reaper][offreactor]")
{
    // The mirror, and the direction where lowering the count would be the use-after-free: a
    // hop back that threw resumes the frame on the EXECUTOR, so it has not come back. Ending
    // the coroutine there would finish the frame off the reactor and leave the count raised
    // for good -- a stop that waits out its ceiling and ends the process. So the hop is tried
    // again, and the cycle carries on from the reactor.
    Fixture f;
    CapturingLogger logger;
    ThreadPoolExecutor pool { 1 };
    ParkingReactor reactor { f.reactor };
    ReaperOnceBack reaper { f.storage, logger };
    reactor.RefuseNextSubmit();
    reaper->Start(reactor, pool);
    // The refused hop back, its retry, and the NEXT sweep's hop back, which only a frame that
    // came back to the reactor can make.
    REQUIRE(TickUntil(
        f,
        "three hop-back submits: the refused one, its retry, and the next sweep's",
        [&] { return reactor.ParkedWorkSubmits() >= 3; },
        [&] { return std::format("{}; {}", Describe(reactor), Describe(*reaper)); }));

    // `ParkedWorkSubmits` counts a `Submit` on ARRIVAL, so the third hop back may still be inside
    // it with the count raised; `Stop`'s held drain waits for it rather than racing the ceiling.
    REQUIRE(reaper.Stop());

    CHECK(reactor.Refused() == 1);
    CHECK_FALSE(std::ranges::any_of(logger.Snapshot(), [](auto const& record) { return record.level == LogLevel::Error; }));
}
