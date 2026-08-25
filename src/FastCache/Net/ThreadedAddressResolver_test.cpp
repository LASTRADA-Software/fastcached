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
