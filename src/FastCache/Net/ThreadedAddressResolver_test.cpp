// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/ThreadedAddressResolver.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include <tests/FrameSentinel.hpp>
#include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

namespace
{

/// A blocking resolver under test control.
///
/// It records the thread it ran on, which is how a case asserts that a lookup
/// was offloaded rather than merely answered -- the count alone would pass
/// against an implementation that resolved inline and incremented a counter.
class ScriptedResolver final: public FastCache::IAddressResolver
{
  public:
    std::expected<std::vector<FastCache::ResolvedEndpoint>, std::string> Resolve(std::string_view host,
                                                                                 std::uint16_t port) override
    {
        std::ignore = port;
        {
            std::scoped_lock const guard { _mutex };
            _calls.emplace_back(host);
            _threads.insert(std::this_thread::get_id());
        }

        // Hold here until released, so a case can observe the queue while a
        // lookup is genuinely in flight. Without a latch there is no way to be
        // inside the window a bounded queue exists to bound.
        std::unique_lock lock { _gateMutex };
        _gate.wait(lock, [this] { return !_held; });
        lock.unlock();

        if (_fail)
            return std::unexpected(std::string { "scripted failure" });
        return std::vector<FastCache::ResolvedEndpoint> { FastCache::ResolvedEndpoint {} };
    }

    /// Releasing here is not tidiness. A held gate keeps a worker inside
    /// `Resolve`, where nothing can reach it, so the resolver's join waits
    /// forever -- and a Catch2 `REQUIRE` that fires before the test's own
    /// `Release()` unwinds straight past it. That turns any single failed
    /// assertion into a suite timeout naming nothing, which is the trap this
    /// repository has already paid for once.
    ScriptedResolver() = default;
    ScriptedResolver(ScriptedResolver const&) = delete;
    ScriptedResolver(ScriptedResolver&&) = delete;
    ScriptedResolver& operator=(ScriptedResolver const&) = delete;
    ScriptedResolver& operator=(ScriptedResolver&&) = delete;

    ~ScriptedResolver() override
    {
        Release();
    }

    void Hold() noexcept
    {
        std::scoped_lock const guard { _gateMutex };
        _held = true;
    }

    void Release() noexcept
    {
        {
            std::scoped_lock const guard { _gateMutex };
            _held = false;
        }
        _gate.notify_all();
    }

    void Fail() noexcept
    {
        _fail = true;
    }

    [[nodiscard]] std::size_t Calls() const
    {
        std::scoped_lock const guard { _mutex };
        return _calls.size();
    }

    [[nodiscard]] bool RanOn(std::thread::id id) const
    {
        std::scoped_lock const guard { _mutex };
        return _threads.contains(id);
    }

    [[nodiscard]] std::size_t DistinctThreads() const
    {
        std::scoped_lock const guard { _mutex };
        return _threads.size();
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::string> _calls;
    std::set<std::thread::id> _threads;

    std::mutex _gateMutex;
    std::condition_variable _gate;
    bool _held { false };
    std::atomic<bool> _fail { false };
};

/// Drives one resolution and records the outcome.
FastCache::Task<void> Lookup(FastCache::IAsyncAddressResolver* resolver,
                             std::string host,
                             std::uint16_t port,
                             FastCache::IReactor* reactor,
                             std::optional<FastCache::ResolveResult>* out)
{
    *out = co_await resolver->Resolve(std::move(host), port, reactor);
    co_return;
}

/// Drain until the task finishes or the budget runs out.
///
/// Bounded rather than looping forever: a hand-back that never arrives is the
/// defect these cases exist to catch, and an unbounded wait would report it as a
/// suite timeout naming nothing.
bool DrainUntil(FastCache::TestReactor& reactor, std::optional<FastCache::ResolveResult> const& out)
{
    for (auto attempt = 0; attempt < 2000; ++attempt)
    {
        reactor.Drain();
        if (out.has_value())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

/// Unblocks and joins a thread running `ThreadedAddressResolver::Stop()`, however
/// the case around it ends.
///
/// Not tidiness, and the same trap ScriptedResolver's destructor records, reached
/// from the other side: a Catch2 `REQUIRE` firing between the thread's start and
/// its join unwinds into `std::thread`'s destructor while it is still joinable,
/// and that calls `std::terminate()`. One reportable assertion failure would
/// become an abort of the whole binary naming nothing.
///
/// Releasing the gate FIRST is what lets the join finish at all: `Stop()` is
/// waiting on the worker that the gate is holding. Declare this after the thread,
/// so it is destroyed before it.
class GateReleasingJoin
{
  public:
    /// @param inner   The gate holding the worker `Stop()` is waiting on.
    /// @param stopper The thread running `Stop()`.
    GateReleasingJoin(ScriptedResolver& inner, std::thread& stopper) noexcept:
        _inner { inner },
        _stopper { stopper }
    {
    }

    GateReleasingJoin(GateReleasingJoin const&) = delete;
    GateReleasingJoin(GateReleasingJoin&&) = delete;
    GateReleasingJoin& operator=(GateReleasingJoin const&) = delete;
    GateReleasingJoin& operator=(GateReleasingJoin&&) = delete;

    ~GateReleasingJoin()
    {
        // Both idempotent, so the case may still do this itself on the happy path
        // where the ordering is part of what it is asserting.
        _inner.Release();
        if (_stopper.joinable())
            _stopper.join();
    }

  private:
    ScriptedResolver& _inner;
    std::thread& _stopper;
};

} // namespace

TEST_CASE("A literal host never reaches the pool", "[net][resolve]")
{
    // The property the fast path exists for. Every internal dial here is to a
    // literal and the launcher makes one per translation unit, so paying a thread
    // hand-off for inet_pton would be a real regression on the build's hot path.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedResolver inner;
    FastCache::ThreadedAddressResolver resolver { inner };

    std::optional<FastCache::ResolveResult> out;
    auto task = Lookup(&resolver, "127.0.0.1", 6674, &reactor, &out);
    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(out.has_value());
    CHECK(FastCache::Testing::Unwrap(out).has_value());
    CHECK(resolver.Offloaded() == 0);
    // Answered on the caller's own thread, so no thread was ever started.
    CHECK(inner.RanOn(std::this_thread::get_id()));
    CHECK(inner.DistinctThreads() == 1);
}

TEST_CASE("A name is resolved off the calling thread and handed back through the reactor", "[net][resolve]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedResolver inner;
    FastCache::ThreadedAddressResolver resolver { inner };

    std::optional<FastCache::ResolveResult> out;
    auto task = Lookup(&resolver, "cache.example.com", 6674, &reactor, &out);
    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(DrainUntil(reactor, out));
    REQUIRE(out.has_value());
    CHECK(FastCache::Testing::Unwrap(out).has_value());
    CHECK(resolver.Offloaded() == 1);

    // The whole point: the lookup did NOT run on the thread that asked for it.
    // A count alone would pass against an implementation that resolved inline.
    CHECK_FALSE(inner.RanOn(std::this_thread::get_id()));
}

TEST_CASE("A caller with no reactor is answered inline", "[net][resolve]")
{
    // There is nowhere to Submit a result back to, so offloading would park a
    // coroutine nothing could resume. Resolving inline is the only answer that
    // works -- and it is what keeps SyncRun sound over this resolver, which is
    // how every blocking-thread caller drives it.
    ScriptedResolver inner;
    FastCache::ThreadedAddressResolver resolver { inner };

    std::optional<FastCache::ResolveResult> out;
    FastCache::SyncRun(Lookup(&resolver, "cache.example.com", 6674, nullptr, &out));

    REQUIRE(out.has_value());
    CHECK(FastCache::Testing::Unwrap(out).has_value());
    CHECK(resolver.Offloaded() == 0);
    CHECK(inner.RanOn(std::this_thread::get_id()));
}

TEST_CASE("A full queue is refused rather than waited on", "[net][resolve]")
{
    // Blocking the caller to wait for room would reintroduce, on the reactor
    // thread, exactly the stall this class removes. And an unbounded queue is a
    // memory-exhaustion hole reachable by whatever provokes dials.
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedResolver inner;
    inner.Hold();
    FastCache::ThreadedAddressResolver resolver { inner,
                                                  FastCache::ThreadedResolverOptions { .threads = 1, .maxQueueDepth = 1 } };

    // One lookup occupies the single worker; the next fills the single queue slot.
    std::optional<FastCache::ResolveResult> first;
    auto firstTask = Lookup(&resolver, "one.example.com", 1, &reactor, &first);
    reactor.Submit(firstTask.Native());
    reactor.Drain();

    // Wait for the worker to actually DEQUEUE the first job before offering the
    // second. Ordering, not politeness: until it does, the first job is still
    // occupying the single queue slot, so the second would be the one refused and
    // the third would be quietly accepted -- the case would then be asserting the
    // opposite of what it says.
    for (auto attempt = 0; attempt < 2000 && inner.Calls() == 0; ++attempt)
        std::this_thread::sleep_for(1ms);
    REQUIRE(inner.Calls() == 1);

    std::optional<FastCache::ResolveResult> second;
    auto secondTask = Lookup(&resolver, "two.example.com", 2, &reactor, &second);
    reactor.Submit(secondTask.Native());
    reactor.Drain();
    REQUIRE_FALSE(second.has_value()); // queued, not refused

    std::optional<FastCache::ResolveResult> third;
    auto thirdTask = Lookup(&resolver, "three.example.com", 3, &reactor, &third);
    reactor.Submit(thirdTask.Native());
    reactor.Drain();

    REQUIRE(third.has_value());
    REQUIRE_FALSE(FastCache::Testing::Unwrap(third).has_value());
    // WouldBlock and not SystemError: a caller can retry the first and can do
    // nothing at all with the second.
    CHECK(FastCache::Testing::Unwrap(third).error().code == FastCache::NetErrorCode::WouldBlock);
    CHECK(resolver.Refused() == 1);

    inner.Release();
    REQUIRE(DrainUntil(reactor, first));
    REQUIRE(DrainUntil(reactor, second));
}

TEST_CASE("Stopping resumes a queued lookup rather than stranding it", "[net][resolve]")
{
    FastCache::ManualClock clock;
    FastCache::TestReactor reactor { clock };
    ScriptedResolver inner;
    inner.Hold();

    // Everything lives to the end of the case, deliberately. A `Task` destroyed
    // while its coroutine is still suspended frees a frame the reactor is still
    // holding a handle to, and the next Drain resumes freed memory -- which is
    // the same lifetime rule `AsyncQueue` states for its waiter, reached from the
    // other side. So both lookups are driven to completion before anything here
    // goes out of scope.
    FastCache::ThreadedAddressResolver resolver { inner,
                                                  FastCache::ThreadedResolverOptions { .threads = 1, .maxQueueDepth = 8 } };

    std::optional<FastCache::ResolveResult> first;
    auto firstTask = Lookup(&resolver, "one.example.com", 1, &reactor, &first);
    reactor.Submit(firstTask.Native());
    reactor.Drain();
    for (auto attempt = 0; attempt < 2000 && inner.Calls() == 0; ++attempt)
        std::this_thread::sleep_for(1ms);
    REQUIRE(inner.Calls() == 1);

    std::optional<FastCache::ResolveResult> queued;
    auto queuedTask = Lookup(&resolver, "two.example.com", 2, &reactor, &queued);
    reactor.Submit(queuedTask.Native());
    reactor.Drain();
    REQUIRE_FALSE(queued.has_value());

    // Stop() has to observe the queued lookup STILL QUEUED, and it cannot run on
    // this thread to do it: Stop() joins the pool, and the only worker is not free
    // to be joined until this thread releases the lookup it is held inside. So
    // Stop() goes on a thread of its own, and the in-flight lookup is released
    // only once Stop() has drained the queue -- observable from here because
    // settling the abandoned lookup Submits its waiter, and TestReactor guards
    // both Submit and PendingSubmissions with the same mutex.
    //
    // Releasing BEFORE Stop() is a race rather than an ordering, and it is the one
    // this case exists to be sure of: the worker wakes, finds `stopping` still
    // false and the queue non-empty, dequeues the very job Stop() was about to
    // cancel, and answers it RESOLVED -- so the assertion below reads `.error()`
    // off a success. Idle machines win that race nearly always, which is why it
    // passed here and failed on three loaded CI runners at once.
    std::thread stopper { [&resolver] { resolver.Stop(); } };
    GateReleasingJoin const finish { inner, stopper }; // after `stopper`, so destroyed before it

    for (auto attempt = 0; attempt < 2000 && reactor.PendingSubmissions() == 0; ++attempt)
        std::this_thread::sleep_for(1ms);
    REQUIRE(reactor.PendingSubmissions() == 1); // the abandoned lookup, settled by Stop()

    // Explicit here as well as in `finish`, because the REST of this case depends
    // on the ordering: the in-flight lookup is only settled once the gate opens,
    // and the drain below asserts that it was.
    inner.Release();
    stopper.join();

    REQUIRE(DrainUntil(reactor, queued));
    REQUIRE(FastCache::Testing::Unwrap(queued).error().code == FastCache::NetErrorCode::Cancelled);

    // The in-flight one is driven to completion too, so no frame is destroyed
    // suspended when this scope ends.
    REQUIRE(DrainUntil(reactor, first));
    CHECK(reactor.PendingSubmissions() == 0);
}

TEST_CASE("A lookup failure names what could not be resolved", "[net][resolve]")
{
    // "resolution failed" without the host is the one thing the reader already
    // knew, so the message carries host and port and the code is one a connector
    // can act on.
    ScriptedResolver inner;
    inner.Fail();
    FastCache::ThreadedAddressResolver resolver { inner };

    std::optional<FastCache::ResolveResult> out;
    FastCache::SyncRun(Lookup(&resolver, "nowhere.example.com", 6674, nullptr, &out));

    REQUIRE(out.has_value());
    REQUIRE_FALSE(FastCache::Testing::Unwrap(out).has_value());
    CHECK(FastCache::Testing::Unwrap(out).error().code == FastCache::NetErrorCode::AddressNotAvail);
    CHECK(FastCache::Testing::Unwrap(out).error().context.contains("nowhere.example.com"));
    CHECK(FastCache::Testing::Unwrap(out).error().context.contains("6674"));
}

// ---------------------------------------------------------------------------
// #1041: what the hand-back owes a chain nobody dequeues.
//
// `Settle` hands the waiter over with `reactor->Submit(waiter)` -- a BORROWED
// handle, erased in `SlotPark::await_suspend` long before the resolver sees it,
// so a reactor destroyed before that post is dequeued frees nothing. Correct for
// a lookup some `Task` owns, a leak for a detached one, and indistinguishable at
// the call site today.
//
// `Stop()` is what makes this ORDERED rather than raced: it joins the workers AND
// settles whatever is still queued rather than dropping it, so the hand-back has
// provably happened by the time it returns -- by either route, through the one
// `Settle`. `PendingSubmissions()` is the positive control, because a case whose
// finding is that a frame was NOT freed has to show first that the site it is
// about was reached at all.
namespace
{

using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;

/// A chain owned by NOBODY, parked on a lookup a worker thread will settle.
FastCache::DetachedTask ResolveDetached(FastCache::IAsyncAddressResolver* resolver,
                                        FastCache::IReactor* reactor,
                                        FrameSentinel sentinel,
                                        FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto result = co_await resolver->Resolve("cache.example.com", 6674, reactor);
    (void) result;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// The same shape, owned by the `Task` the caller holds. The control.
FastCache::Task<void> ResolveOwned(FastCache::IAsyncAddressResolver* resolver,
                                   FastCache::IReactor* reactor,
                                   FrameSentinel sentinel,
                                   FrameCounters* counters)
{
    (void) sentinel;
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    auto result = co_await resolver->Resolve("cache.example.com", 6674, reactor);
    (void) result;
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

} // namespace

TEST_CASE("A detached lookup handed back and never dequeued is freed at teardown", "[net][resolve][parkedwork]")
{
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        ScriptedResolver inner;

        // HELD before the lookup starts, and that is what makes this case
        // deterministic (#1194). `Settle` hands the waiter back only if one is
        // REGISTERED, and `slot.waiter` is written in `SlotPark::await_suspend` --
        // so a worker that dequeues and settles before the coroutine suspends finds
        // an empty waiter, submits NOTHING, and the body resumes inline through the
        // already-done path. `PendingSubmissions()` then reads 0 and this case's
        // premise never held. Seen once on a hosted runner and not once in 400 runs
        // here, which is the rate that makes it read as somebody else's regression.
        //
        // `counters.parked` cannot close that window while reading exactly as
        // though it does: it is incremented on the statement BEFORE the await.
        // `Stop()` cannot either -- it orders the hand-back against its own return,
        // and the worker is free to settle long before it is called.
        inner.Hold();
        FastCache::ThreadedAddressResolver resolver { inner };

        ResolveDetached(&resolver, &reactor, FrameSentinel { &counters }, &counters);
        REQUIRE(counters.parked == 1);

        // The gate's effect, asserted rather than assumed: with a worker held
        // inside `Resolve` nothing can have been handed back yet. Without this the
        // case cannot tell "the gate held the window open" from "the hand-back
        // happened early and the count below is a coincidence".
        REQUIRE(reactor.PendingSubmissions() == 0);

        // Released BEFORE `Stop()`, never after: a held gate keeps the worker inside
        // `Resolve`, where the join below would wait on it forever.
        inner.Release();

        // A name rather than a literal, or the fast path answers inline and no
        // worker -- and so no hand-back -- ever happens.
        resolver.Stop();

        REQUIRE(reactor.PendingSubmissions() == 1);
        REQUIRE(counters.completed == 0);
        // The reactor is destroyed WITHOUT draining, so the post is never dequeued.
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A lookup some Task owns is left alone by the reactor", "[net][resolve][parkedwork]")
{
    // The control, and it is mandatory rather than decoration: freeing everything
    // at teardown is as wrong as freeing nothing, and this is the case a reactor
    // that freed what it merely borrows would double free in.
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        FastCache::TestReactor reactor { clock };
        ScriptedResolver inner;
        FastCache::ThreadedAddressResolver resolver { inner };

        auto lookup = ResolveOwned(&resolver, &reactor, FrameSentinel { &counters }, &counters);

        // HELD across the drain, and that is the whole repair (#1149).
        //
        // `Stop()` settles the jobs still QUEUED on the calling thread, which is
        // ordered. A job a worker has already dequeued is settled on the WORKER
        // thread, at an instant nothing here orders -- and `Settle` ends in
        // `reactor->Submit(...)`. So the hand-back can land while this thread is
        // still inside `Drain()`, which DEQUEUES AND RESUMES it. The submission is
        // then gone, and the assertion below reads 0.
        //
        // It races INTO the drain rather than arriving after it, so no bound on
        // this thread can order it: the worker is free to settle from the instant
        // it dequeues, which is before `Drain()` is even entered. Lengthening a
        // wait cannot un-consume a submission, and the sibling case above cannot
        // show this because it never drains at all. Holding the resolver is what
        // turns the race into an ordering.
        inner.Hold();
        reactor.Submit(lookup.Native());
        reactor.Drain();
        inner.Release();

        // NO assertion inside the held window, deliberately. A `REQUIRE` firing
        // while the gate is held unwinds straight past `Release()`, and
        // `ThreadedAddressResolver`'s destructor then joins a worker parked inside
        // `Resolve` forever -- a suite timeout naming nothing, which this file's own
        // `ScriptedResolver` comment says the project has already paid for once.
        // Removing the window is better than guarding it.
        resolver.Stop();

        // `parked` is incremented at the top of the coroutine body, BEFORE the
        // `co_await`, so it reads 1 under either ordering and is evidence that the
        // body ran rather than evidence about the hand-back.
        REQUIRE(counters.parked == 1);
        REQUIRE(reactor.PendingSubmissions() == 1);
        CHECK(counters.destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(counters.destroyed == 1);
}
