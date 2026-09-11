// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/IocpConnector.hpp>

#if defined(_WIN32)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Async/IReactor.hpp>
    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/ParkedWork.hpp>
    #include <FastCache/Async/ResumeOn.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/IocpDial.hpp>
    #include <FastCache/Net/IocpSocket.hpp>
    #include <FastCache/Net/TcpClient.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <algorithm>
    #include <array>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <ranges>
    #include <span>
    #include <string>
    #include <tuple>
    #include <vector>

    #include <tests/FrameSentinel.hpp>
    #include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

namespace
{

/// Read `want` bytes and record them.
///
/// Its own task so it can PARK on an empty socket while the caller goes on to
/// send. A read that finds data or EOF already waiting proves nothing about the
/// completion port; only one with nothing to return does.
FastCache::DetachedTask ReadInto(
    FastCache::ISocket* client, std::size_t want, std::vector<std::byte>* seen, bool* done, std::string* why)
{
    std::vector<std::byte> buffer(want);
    auto const read = co_await client->Read(buffer);
    if (read.has_value())
    {
        buffer.resize(*read);
        *seen = buffer;
        *why = "read " + std::to_string(*read);
    }
    else
        *why = read.error().ToString();
    *done = true;
    co_return;
}

/// Accept one connection and send the payload on it.
///
/// A task of its own, started BEFORE the dial. On IOCP an accept must be awaited
/// by somebody while it is outstanding: `IocpListener::Accept` issues AcceptEx
/// immediately but its op records the awaitable only in the suspend callback, so
/// a completion that arrives before anyone awaits is dropped and the accept never
/// resolves. Creating the awaitable early and awaiting it after the dial -- which
/// reads naturally and works on epoll -- deadlocks here.
FastCache::DetachedTask AcceptAndSend(FastCache::IocpListener* server, std::span<std::byte const> payload, std::string* why)
{
    auto accepted = co_await server->Accept();
    if (!accepted.has_value())
    {
        *why = "accept failed: " + accepted.error().ToString();
        co_return;
    }
    if (!co_await FastCache::SendAll(accepted.value().get(), payload))
        *why = "send failed";
    accepted.value()->Close();
    co_return;
}

/// Dial through ConnectEx and read what the acceptor sends.
///
/// A free function taking raw pointers rather than a capturing lambda: a
/// coroutine closure outlives the expression that created it.
FastCache::DetachedTask DriveExchange(FastCache::IocpReactor* loop,
                                      FastCache::IocpConnector* dialer,
                                      FastCache::IocpListener* server,
                                      std::uint16_t port,
                                      std::optional<FastCache::SocketResult>* out,
                                      std::vector<std::byte>* seen,
                                      std::string* why)
{
    *out = co_await dialer->Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = 5s });
    auto& dialResult = out->value();
    if (dialResult.has_value() && dialResult.value() != nullptr)
    {
        auto readDone = false;
        ReadInto(dialResult.value().get(), 4, seen, &readDone, why);

        while (!readDone)
            co_await FastCache::ResumeOn { *loop };

        dialResult.value()->Close();
    }
    server->Close();
    loop->Stop();
    co_return;
}

} // namespace

TEST_CASE("A ConnectEx dial connects and then actually transfers bytes", "[net][iocpconnector]")
{
    // Not "did it connect": ConnectEx leaves the handle's context unset until
    // SO_UPDATE_CONNECT_CONTEXT is applied, so a socket that skipped that step
    // reports success and then fails every ordinary call made on it. Only moving
    // bytes through the returned socket distinguishes the two -- and the read is
    // arranged to park, so it also proves the handle really is on the port.
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };

    auto listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("no loopback listener available on this host");

    FastCache::InlineAddressResolver resolver;
    FastCache::IocpConnector connector { reactor, resolver, clock };

    std::optional<FastCache::SocketResult> dialed;
    std::vector<std::byte> echoed;
    std::string why;

    constexpr std::array<std::byte, 4> payload {
        std::byte { 'p' }, std::byte { 'i' }, std::byte { 'n' }, std::byte { 'g' }
    };
    AcceptAndSend(listener.get(), payload, &why);
    DriveExchange(&reactor, &connector, listener.get(), listener->BoundPort(), &dialed, &echoed, &why);

    // Bounded: a completion that never arrives -- the shape every mistake here
    // produces -- would otherwise report as a suite timeout naming nothing.
    FastCache::DeadlineTimer const watchdog {
        reactor, clock.Now() + 15s, [](void* state) { static_cast<FastCache::IocpReactor*>(state)->Stop(); }, &reactor
    };
    reactor.Run();

    REQUIRE(dialed.has_value());
    auto const& outcome = FastCache::Testing::Unwrap(dialed);
    INFO("dial outcome: " << (outcome.has_value() ? std::string { "connected" } : outcome.error().ToString()));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value() != nullptr);
    INFO("exchange: " << why);
    CHECK(echoed.size() == 4);
}

// ---------------------------------------------------------------------------
// #1138: what a settled ConnectEx dial owes a chain nobody dequeues.
//
// The IOCP half of #1041. `PlatformConnector_test.cpp` covers `SettleDial` and
// says in its own `#else` arm that it reaches nothing here -- because until #1138
// the op, the awaitable and the hand-back all lived in an anonymous namespace
// inside `IocpConnector.cpp` and no other translation unit could name them. They
// are in `Net/IocpDial.hpp` now, and this is the demonstration.
//
// **What is under test is the real `Detail::SettleConnect`**, instantiated over a
// recording reactor exactly as `IocpConnector.cpp` instantiates it over
// `IocpReactor`. What is modelled is the reactor's teardown.

namespace
{

using FastCache::Testing::FrameCounters;
using FastCache::Testing::FrameSentinel;

/// A reactor that runs no loop, records what it is handed, and models teardown.
///
/// **Why a double rather than the real `IocpReactor`, measured rather than
/// preferred.** The end-to-end window is a race between two posted packets: a
/// completion-driven settle posts a *new* `KeyResumeCoroutine`, which by
/// construction cannot be in the batch being processed, so it leaks only if the
/// loop exits before dequeuing it -- and `RunLoop`'s `if (stopRequested) return;`
/// fires only when `Stop()`'s `KeyStop` packet landed in that same batch. Whether
/// it does is decided by the completion port's ordering of two posts, which a test
/// can neither arrange nor observe. With no loop, nothing dispatches and the case
/// drives the settle itself, so the ordering is not a race at all.
///
/// **It is narrower than a real reactor, never wider.** It runs no loop, so
/// nothing it is handed is ever resumed; it takes no locks, because a case drives
/// it from one thread where a real `Submit` is callable from any; and it answers
/// `TeardownIsSerialisedWithDispatch()` through the `!Running()` arm, which is the
/// truth here rather than a shortcut. It accepts no `Submit` shape a real reactor
/// refuses -- *a fake more permissive than the thing it stands for* is a family
/// this project has paid for repeatedly, and cheapness is a reason it was easy to
/// write rather than a reason it is faithful.
///
/// **Not shared with `PlatformConnector_test.cpp`'s `RecordingDialReactor`, and
/// that is a decision rather than an oversight.** The two are platform-exclusive
/// by `#if`, so they can never both compile in one build and cannot disagree at
/// run time; this one owes no handler seam at all, being a strict subset of the
/// other. Consolidating them under `src/tests/` is the right end state and is a
/// cross-platform change of its own rather than a rider on this one.
class RecordingConnectReactor final: public FastCache::IReactor
{
  public:
    /// @param clock Clock this reactor reports; nothing on this path reads it.
    explicit RecordingConnectReactor(FastCache::IClock& clock) noexcept:
        _clock { clock }
    {
    }

    void Stop() noexcept override {}

    void Submit(std::coroutine_handle<> handle) override
    {
        _borrowed.push_back(handle);
    }

    void Submit(FastCache::ParkedWork work) override
    {
        // Recorded separately, because `Detail::Parked` deliberately exposes only
        // the handle it would resume -- the root it may free is not something a
        // consumer is meant to read, and this double is the one place that has to.
        _handedOwned.push_back(static_cast<bool>(work.abandon));
        _handed.emplace_back(work);
    }

    void Schedule(FastCache::TimePoint /*deadline*/, std::coroutine_handle<> handle) override
    {
        // RECORDED rather than dropped, and that is the difference between a double
        // and a more permissive stand-in: a real reactor queues this, so a double
        // that silently discarded it would make a caller that used the borrowing
        // form look correct here and leak in production.
        _borrowedTimers.push_back(handle);
    }

    void Schedule(FastCache::TimePoint /*deadline*/, FastCache::ParkedWork work) override
    {
        _scheduled.emplace_back(work);
    }

    /// Retract a pending resumption, the way every real reactor here does.
    ///
    /// A blanket `false` would be legal -- `DeadlineTimer` documents *cannot
    /// retract* as a supported reply -- and it would make this double answer
    /// differently from every reactor it stands for. `Take()` before erasing,
    /// because after this the caller is the only one who may resume or destroy the
    /// chain, so the entry must do neither on its way out.
    /// @param handle A handle previously submitted or scheduled here.
    /// @return Whether it was found and retracted.
    [[nodiscard]] bool CancelPending(std::coroutine_handle<> handle) noexcept override
    {
        if (!handle)
            return false;
        for (auto* container: { &_handed, &_scheduled })
        {
            auto const found = std::ranges::find(*container, handle, &FastCache::Detail::Parked::Handle);
            if (found != container->end())
            {
                std::ignore = found->Take();
                container->erase(found);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] FastCache::IClock& Clock() noexcept override
    {
        return _clock;
    }

    /// @return How many chains were handed over through the OWNING overload.
    [[nodiscard]] std::size_t Handed() const noexcept
    {
        return _handed.size();
    }

    /// @return How many were handed over as borrowed handles.
    [[nodiscard]] std::size_t Borrowed() const noexcept
    {
        return _borrowed.size();
    }

    /// @return Whether the most recent hand-back named a chain root to free.
    [[nodiscard]] bool LastHandedCarriedOwnership() const noexcept
    {
        return !_handedOwned.empty() && _handedOwned.back();
    }

  protected:
    void RunLoop() override {}

  private:
    FastCache::IClock& _clock;
    std::vector<std::coroutine_handle<>> _borrowed;
    std::vector<std::coroutine_handle<>> _borrowedTimers;
    std::vector<FastCache::Detail::Parked> _handed;
    std::vector<bool> _handedOwned;
    std::vector<FastCache::Detail::Parked> _scheduled;
};

using RecordingConnectOp = FastCache::Detail::ConnectOp<RecordingConnectReactor>;

/// Park on a dial from a chain nothing owns.
///
/// A free function taking raw pointers rather than a capturing lambda: a coroutine
/// closure outlives the expression that created it. `FrameSentinel` is taken BY
/// VALUE so it lives in the frame rather than in the caller.
/// @param op The dial to park on.
/// @param counters Where entry, completion and destruction are tallied.
FastCache::DetachedTask ParkDetached(RecordingConnectOp* op, FrameSentinel /*sentinel*/, FrameCounters* counters)
{
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await FastCache::Detail::ConnectPark<RecordingConnectOp> { .op = op };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

/// The same park, from a chain a `Task` owns.
/// @param op The dial to park on.
/// @param counters Where entry, completion and destruction are tallied.
FastCache::Task<void> ParkOwned(RecordingConnectOp* op, FrameSentinel /*sentinel*/, FrameCounters* counters)
{
    counters->parked.fetch_add(1, std::memory_order_acq_rel);
    co_await FastCache::Detail::ConnectPark<RecordingConnectOp> { .op = op };
    counters->completed.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

} // namespace

TEST_CASE("A detached ConnectEx dial handed back and never dequeued is freed at teardown",
          "[net][iocpconnector][parkedwork]")
{
    FrameCounters counters;
    {
        // A ManualClock rather than a SteadyClock, and the choice is the assertion:
        // nothing on this path reads the clock at all, and the fake that cannot
        // advance on its own is how the file says so.
        FastCache::ManualClock clock;
        RecordingConnectReactor reactor { clock };
        RecordingConnectOp op;
        op.reactor = &reactor;

        ParkDetached(&op, FrameSentinel { &counters }, &counters);

        // The premise, asserted AT THE OP rather than inferred from a counter.
        // `FrameCounters::parked` says the body was ENTERED, which is not that it
        // suspended (#1194); the registered waiter is the fact this case needs, and
        // it is readable here because the case owns the op.
        REQUIRE(counters.parked == 1);
        REQUIRE(op.waiter.resume);
        REQUIRE(counters.completed == 0);

        // Asserted as totals rather than as deltas, and the precondition is what
        // makes that sound: nothing else submits to this reactor. The readiness case
        // next door has to measure a delta because arming its dial puts
        // `DeadlineTimer`'s own coroutine on the reactor too; there is no timer on
        // this path.
        REQUIRE(reactor.Handed() == 0);
        REQUIRE(reactor.Borrowed() == 0);

        // The site under test. Nothing dispatches here, so this IS the settle rather
        // than a race with one.
        FastCache::Detail::SettleConnect(op, 0);

        // The positive control: the hand-back happened at all. A case whose finding
        // is that a frame was NOT freed has to show first that the site it is about
        // was reached.
        REQUIRE(reactor.Handed() + reactor.Borrowed() == 1);

        // What DISTINGUISHES. Under `Submit(waiter.resume)` the settle reaches
        // `Submit` just the same and the line above still passes -- the chain goes
        // over as a BORROWED handle, which frees nothing.
        CHECK(reactor.Borrowed() == 0);
        CHECK(reactor.Handed() == 1);
        CHECK(reactor.LastHandedCarriedOwnership());
        REQUIRE(counters.completed == 0);

        // The reactor is destroyed WITHOUT dequeuing, exactly as a stopped one is.
    }
    CHECK(counters.destroyed == 1);
}

TEST_CASE("A ConnectEx dial some Task owns is left alone by the reactor", "[net][iocpconnector][parkedwork]")
{
    // The control, and it is mandatory rather than decoration: freeing everything at
    // teardown is as wrong as freeing nothing, and this is the case a reactor that
    // freed what it merely borrows would double free in. It must stay GREEN under
    // the change that makes the case above fail.
    FrameCounters counters;
    {
        FastCache::ManualClock clock;
        RecordingConnectReactor reactor { clock };
        RecordingConnectOp op;
        op.reactor = &reactor;

        auto parked = ParkOwned(&op, FrameSentinel { &counters }, &counters);
        parked.Native().resume();

        REQUIRE(counters.parked == 1);
        REQUIRE(op.waiter.resume);
        REQUIRE(reactor.Handed() == 0);
        REQUIRE(reactor.Borrowed() == 0);

        FastCache::Detail::SettleConnect(op, 0);

        // The same one hand-back. What this case then asserts is BEHAVIOUR -- the
        // frame outlives the reactor -- rather than the shape of what was handed
        // over: an assertion on the shape here would go red under the very break the
        // case above exists to catch, and a control that fails under the break is
        // not a control.
        REQUIRE(reactor.Handed() + reactor.Borrowed() == 1);
        CHECK(counters.destroyed == 0);
    }
    // Freed exactly once, by the Task that owns it.
    CHECK(counters.destroyed == 1);
}

#endif // _WIN32
