// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/SleepUntil.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/PlatformListener.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>

/// What `ISocket::CancelRead` does to a PARKED read, on a REAL socket, on this
/// platform.
///
/// ## Why this file exists
///
/// `CancelRead` is the only spelling of *abandon this read* that is not `Close()`, and
/// its contract on `ISocket` is explicit: **idempotent, and not a `Close()` -- the
/// socket stays open and a later `Read` works.** That header also says, of the
/// transports that park, *"a new one that can must too, and nothing but this sentence
/// enforces that"*.
///
/// Nothing enforced the BEHAVIOUR either, on the platforms the gate runs. Surveyed for
/// [#778](https://github.com/LASTRADA-Software/fastcached/issues/778), the coverage was:
///
///   - `Net/IocpSocket_test.cpp` -- a real socket, and **Windows only**.
///   - `Protocol/RedisResp_test.cpp` -- `Testing::ParkingReadableSocket`, a FAKE that
///     exists precisely because `InMemorySocket::WaitReadable` resolves synchronously.
///
/// So `EpollSocket::CancelRead` and `KqueueSocket::CancelRead` -- two real overrides
/// sharing one `Detail::RetireParkedRead` -- were exercised by nothing on Linux or
/// macOS. That is #778's shape exactly: **a fake that resolves synchronously what
/// production suspends on cannot exercise a suspension protocol**, and the suite is
/// green whether the protocol is implemented or not.
///
/// Cross-platform rather than under an `#if`, for the reason `WaitReadable_test.cpp`
/// gives: it drives `PlatformReactor` / `PlatformListener`, so each CI leg puts its own
/// implementation through these same assertions, and a platform that disagrees is
/// visible rather than untested.
///
/// ## Two cancels in a row are NOT two no-ops, and that is measured
///
/// The contract's word is *idempotent*, and the reading it invites -- **call it twice,
/// the second does nothing** -- is false on epoll and kqueue. `RetireParkedRead`
/// completes the parked awaitable, and on those transports a completion resumes the
/// awaiting coroutine **inline**; that coroutine runs on to its next `Read` and arms
/// it before the caller's following statement executes. The second `CancelRead` then
/// cancels a read that did not exist when it was written.
///
/// Measured here rather than reasoned about: the first draft of this file cancelled
/// twice, and the read issued after the cancel came back `Cancelled` (code 2) instead
/// of the two bytes the peer had sent. On IOCP the same two lines behave differently
/// again, because there a completion is marshalled to a later turn rather than
/// resumed inline -- so the reader has not re-armed when the second call runs.
///
/// What *is* idempotent is a cancel with **nothing parked**, which is the case below,
/// and it is the shape the contract is actually about: `RunBlockingRead` retires by
/// RAII as well as explicitly, so the redundant call happens with the slot already
/// empty rather than with a fresh read in it.
///
/// ## Why there is no timing race here
///
/// Both coroutines run on the ONE reactor thread. The reader publishes `arming` and
/// then `co_await`s a read that must park -- the client sends nothing -- so control
/// returns to the reactor before anything else runs. The canceller therefore cannot
/// observe `arming` unless the read is ALREADY parked. No sleep-and-hope, and the case
/// records the fact rather than assuming it.
namespace
{

/// What the reader observed, published across the reactor and client threads.
struct Observation
{
    /// The accepted socket, for the canceller. A plain pointer: both coroutines run on
    /// the reactor thread, and the reader outlives the canceller by construction.
    FastCache::ISocket* socket { nullptr };

    /// Set immediately before the read that must park.
    std::atomic<bool> arming { false };

    /// Whether the FIRST read had already resolved when the canceller ran. **The
    /// assertion that stops this case being vacuous**: if the read resolved
    /// synchronously, nothing was ever parked and every check below would hold for a
    /// reason unrelated to cancellation.
    std::atomic<bool> firstResolvedAtCancel { true };

    /// Set once the first read has returned, whatever it returned.
    std::atomic<bool> firstResolved { false };

    /// What the first read came back as.
    std::atomic<bool> firstHasValue { true };
    std::atomic<int> firstErrorCode { -1 };

    /// What a read issued AFTER the cancel came back as -- the half that separates
    /// `CancelRead` from `Close`. The error code is carried so a failure says WHICH
    /// way it went rather than only that it went.
    std::atomic<bool> secondHasValue { false };
    std::atomic<std::size_t> secondCount { 0 };
    std::atomic<int> secondErrorCode { -1 };

    /// Set once the whole exchange is done.
    std::atomic<bool> finished { false };
};

/// Accept, park a read, and read again once it has been cancelled.
///
/// By POINTER, never by reference: this is a coroutine, so its frame outlives the
/// expression that created it and a reference parameter would bind to storage the
/// caller may already have released. The rule every coroutine in this tree carries.
/// @param reactor Stopped once the exchange is complete, so `Run()` returns.
/// @param listener Bound listener to accept on.
/// @param out Where the observation is published; must outlive the task.
FastCache::DetachedTask ReadParkThenReadAgain(FastCache::PlatformReactor* reactor,
                                              FastCache::IListener* listener,
                                              Observation* out)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        out->finished.store(true, std::memory_order_release);
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accepted);
    out->socket = socket.get();

    std::array<std::byte, 8> first {};
    out->arming.store(true, std::memory_order_release);
    auto const parked = co_await socket->Read(std::span<std::byte> { first });

    out->firstHasValue.store(parked.has_value(), std::memory_order_relaxed);
    if (!parked.has_value())
        out->firstErrorCode.store(static_cast<int>(parked.error().code), std::memory_order_relaxed);
    out->firstResolved.store(true, std::memory_order_release);

    // **A SECOND read on the same socket, which is legal only because the cancel
    // released the read slot.** A socket has one read operation shared by `Read` and
    // `WaitReadable` (#663), so this arm is itself an assertion: under an
    // implementation that completed the awaitable without detaching it first, this is
    // the double-arm the slot guard aborts on.
    std::array<std::byte, 8> second {};
    auto const after = co_await socket->Read(std::span<std::byte> { second });
    out->secondHasValue.store(after.has_value(), std::memory_order_relaxed);
    if (after.has_value())
        out->secondCount.store(*after, std::memory_order_relaxed);
    else
        out->secondErrorCode.store(static_cast<int>(after.error().code), std::memory_order_relaxed);

    out->finished.store(true, std::memory_order_release);
    socket->Close();
    reactor->Stop();
    co_return;
}

/// Cancel the reader's parked read, ONCE, from the reactor thread.
///
/// Once, not twice -- see the file header. A second call here would cancel the read
/// the reader armed when this one resumed it inline, which is what the first draft did
/// and what it measured.
/// @param reactor The loop both tasks run on.
/// @param out The reader's observation; must outlive the task.
FastCache::DetachedTask CancelWhenParked(FastCache::PlatformReactor* reactor, Observation* out)
{
    while (!out->arming.load(std::memory_order_acquire))
        co_await FastCache::SleepFor(*reactor, std::chrono::milliseconds { 1 });

    // Recorded BEFORE the cancel: the reader set `arming` and then suspended, and this
    // task only runs because it did, so a resolved read here would mean the read never
    // parked at all.
    out->firstResolvedAtCancel.store(out->firstResolved.load(std::memory_order_acquire), std::memory_order_relaxed);

    out->socket->CancelRead();
    co_return;
}

/// Accept, cancel with NOTHING parked, then read normally.
///
/// The contract's idempotence, in the arrangement that actually occurs: the slot is
/// empty, so the call has nothing to retire and must neither fail nor disturb the
/// socket.
/// @param reactor Stopped once the read has returned.
/// @param listener Bound listener to accept on.
/// @param out Where the observation is published; must outlive the task.
FastCache::DetachedTask CancelWithNothingParked(FastCache::PlatformReactor* reactor,
                                                FastCache::IListener* listener,
                                                Observation* out)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        out->finished.store(true, std::memory_order_release);
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accepted);

    // Twice, with the slot empty both times. Neither may throw, and neither may leave
    // the socket unable to read.
    socket->CancelRead();
    socket->CancelRead();
    out->arming.store(true, std::memory_order_release);

    std::array<std::byte, 8> buffer {};
    auto const got = co_await socket->Read(std::span<std::byte> { buffer });
    out->secondHasValue.store(got.has_value(), std::memory_order_relaxed);
    if (got.has_value())
        out->secondCount.store(*got, std::memory_order_relaxed);
    else
        out->secondErrorCode.store(static_cast<int>(got.error().code), std::memory_order_relaxed);

    out->finished.store(true, std::memory_order_release);
    socket->Close();
    reactor->Stop();
    co_return;
}

/// Wait, bounded, for @p flag.
/// @param flag What is being waited for.
/// @return True when it was set inside the bound.
[[nodiscard]] bool WaitForFlag(std::atomic<bool> const& flag)
{
    for (auto spin = 0; spin < 2000; ++spin)
    {
        if (flag.load(std::memory_order_acquire))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 5 });
    }
    return flag.load(std::memory_order_acquire);
}

} // namespace

TEST_CASE("CancelRead retrieves a parked read and leaves the socket usable", "[net][socket][cancelread]")
{
    // Four properties, and each fails under a different wrong implementation:
    //
    //   1. the read was PARKED     -- otherwise the case is vacuous
    //   2. the await RESUMED       -- the frame is not left parked, which is #710/#663
    //   3. it resumed CANCELLED    -- not EOF, which is what a peer leaving looks like
    //                                 and is the confusion `ArmDisconnect` records
    //   4. the socket still WORKS  -- an implementation spelled as `Close()` passes
    //                                 1 through 3 and fails only here
    //
    // Both mutations were run rather than reasoned about, and one of them corrected
    // what this comment first claimed:
    //
    //   `CancelRead` spelled as `Close()`  -- 13 of 17 assertions still pass. Only
    //       property 4 fails, in both cases, with the read after the cancel coming
    //       back error code 5 rather than the bytes. So properties 1 to 3 cannot tell
    //       a cancel from a close, and 4 is the whole of the discrimination.
    //
    //   `CancelRead` as the inherited no-op -- this case FAILS; it does not hang, which
    //       is what the comment here first predicted. The client's bounded wait expires
    //       after about ten seconds, its close turns the still-parked read into an EOF,
    //       and properties 3 and 4 then fail against that EOF (`-1 == 2`, `0 == 2`).
    //       The no-op is caught, late, and by the error CODE rather than by the
    //       resumption -- which is why property 3 is spelled as a code and not as
    //       "resolved at all".
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    Observation observed;
    ReadParkThenReadAgain(&reactor, listener.get(), &observed);
    CancelWhenParked(&reactor, &observed);

    // Recorded here and asserted on the main thread: a `REQUIRE` firing inside a
    // `jthread` body is `std::terminate`, not a failed case.
    std::atomic<bool> connected { false };

    std::jthread client { [port, &observed, &connected] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        connected.store(true, std::memory_order_release);

        // Nothing is sent until the first read has been cancelled, which is what makes
        // that read park rather than complete. Then two bytes, so the read issued after
        // the cancel has something to return.
        if (!WaitForFlag(observed.firstResolved))
            return;

        std::array<std::byte, 2> const payload { std::byte { 'o' }, std::byte { 'k' } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 2> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        (void) WaitForFlag(observed.finished);
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    REQUIRE(connected.load(std::memory_order_acquire));
    REQUIRE(observed.finished.load(std::memory_order_acquire));

    // 1. It really was parked.
    CHECK_FALSE(observed.firstResolvedAtCancel.load(std::memory_order_relaxed));

    // 2 and 3. It resumed, and as a cancellation rather than as EOF or data.
    REQUIRE(observed.firstResolved.load(std::memory_order_acquire));
    CHECK_FALSE(observed.firstHasValue.load(std::memory_order_relaxed));
    CHECK(observed.firstErrorCode.load(std::memory_order_relaxed) == static_cast<int>(FastCache::NetErrorCode::Cancelled));

    // 4. And the socket was still a socket afterwards.
    INFO("read after the cancel: error code " << observed.secondErrorCode.load(std::memory_order_relaxed) << " (Cancelled="
                                              << static_cast<int>(FastCache::NetErrorCode::Cancelled) << ")");
    CHECK(observed.secondHasValue.load(std::memory_order_relaxed));
    CHECK(observed.secondCount.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("CancelRead with nothing parked disturbs nothing", "[net][socket][cancelread]")
{
    // The contract's *idempotent*, in the arrangement that actually occurs -- and NOT
    // the one the word invites, which the file header measures and explains. A cancel
    // on an empty slot must be a no-op rather than an error, a close, or a lost read
    // interest.
    //
    // **This case cannot see a no-op `CancelRead`, and that is not a defect in it.**
    // Measured: under the inherited no-op it passes, correctly -- with nothing parked
    // there is nothing for a real implementation to do either, so the two are
    // indistinguishable here by construction. The case above is what carries that
    // weight. Recorded because two cases sharing a tag invite the reading that they
    // cover the same ground, and a case that cannot fail for a given defect should say
    // which defect that is rather than leave somebody to find out by breaking it.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    Observation observed;
    CancelWithNothingParked(&reactor, listener.get(), &observed);

    std::atomic<bool> connected { false };

    std::jthread client { [port, &observed, &connected] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        connected.store(true, std::memory_order_release);

        // Written only once the server has cancelled and armed, so the read under test
        // is one that PARKED and was then satisfied -- not one that found bytes already
        // waiting and never suspended at all.
        if (!WaitForFlag(observed.arming))
            return;

        std::array<std::byte, 3> const payload { std::byte { 'y' }, std::byte { 'e' }, std::byte { 's' } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 3> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        (void) WaitForFlag(observed.finished);
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    REQUIRE(connected.load(std::memory_order_acquire));
    REQUIRE(observed.finished.load(std::memory_order_acquire));

    INFO("read after two empty cancels: error code " << observed.secondErrorCode.load(std::memory_order_relaxed));
    CHECK(observed.secondHasValue.load(std::memory_order_relaxed));
    CHECK(observed.secondCount.load(std::memory_order_relaxed) == 3);
}
