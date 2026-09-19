// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
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
#include <format>
#include <span>
#include <string>
#include <thread>

#include <tests/BoundedWait.hpp>

using FastCache::Testing::AwaitUntil;
using FastCache::Testing::OffThreadWaits;
using FastCache::Testing::ReactorWaitOptions;
using FastCache::Testing::WaitHangGuard;
using FastCache::Testing::WaitOptions;
using FastCache::Testing::WaitRest;

/// What `ISocket::CancelRead` does to a PARKED read, on a REAL socket, on this
/// platform.
///
/// ## Why this file exists
///
/// `CancelRead` is the only spelling of *abandon this read* that is not `Close()`. Its
/// contract on `ISocket` used to open **"idempotent, and not a `Close()` -- the socket
/// stays open and a later `Read` works"**; the first half of that was false and #1233
/// replaced it, which is what the last case in this file now holds. That header also
/// says, of the transports that park, *"a new one that can must too, and nothing but
/// this sentence enforces that"*.
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
/// of the two bytes the peer had sent.
///
/// **That sentence used to continue "on IOCP the same two lines behave differently
/// again", and the correction is the whole of
/// [#1233](https://github.com/LASTRADA-Software/fastcached/issues/1233).** Marshalling
/// is a property of the OPERATION rather than of the platform, and IOCP has both:
///
///   - a real `Read` settles on a later turn there, which is the claim that was true;
///   - a `WaitReadable` PROBE is retired **inline**, by `IocpSocket::CancelRead`'s own
///     `readPeekOnly` arm, because a zero-byte receive carries no data a settle could
///     preserve. `Net/IocpSocket_test.cpp`'s *"CancelRead retires a parked probe before
///     it returns"* asserts exactly that, with no drain between the call and the check.
///
/// A probe is the only thing this tree ever parks and then cancels in production -- a
/// readability watch, retired by `RedisResp` and `CompileCacheHandler` -- so for the
/// operation the verb exists for, **all three transports agree, and they agree on the
/// surprising behaviour.** The last case in this file is that one, asserted identically
/// on every leg; the first case is the real-`Read` arrangement, where epoll and kqueue
/// resume inline and IOCP does not.
///
/// What *is* idempotent is a cancel with **nothing parked**, which is the second case
/// below, and it is the shape the contract is actually about: `RunBlockingRead` retires
/// by RAII as well as explicitly, so the redundant call happens with the slot already
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

/// What the exchange has reached, for a wait's account.
/// @param observed The reader's observation.
/// @return Its milestones, in words.
[[nodiscard]] std::string Describe(Observation const& observed)
{
    return std::format("reader armed {}, first read resolved {}, exchange finished {}",
                       observed.arming.load(std::memory_order_acquire),
                       observed.firstResolved.load(std::memory_order_acquire),
                       observed.finished.load(std::memory_order_acquire));
}

/// Cancel the reader's parked read, ONCE, from the reactor thread.
///
/// Once, not twice -- see the file header. A second call here would cancel the read
/// the reader armed when this one resumed it inline, which is what the first draft did
/// and what it measured.
///
/// Waits for the reader to arm through `AwaitUntil`, bounded on the reactor's clock
/// (#1453): a reader that never arms is a red naming what was waited for, not a case
/// that spins until ctest kills it. **A wait that ran out ends this task there** -- the
/// cancel it guards would retrieve a read that never parked -- and the client's own wait
/// for the cancel then runs out after it and closes the socket, which ends the reader.
/// @param reactor The loop both tasks run on.
/// @param out The reader's observation; must outlive the task.
/// @param waits Keeps the wait's account for the case; must outlive the task.
FastCache::DetachedTask CancelWhenParked(FastCache::PlatformReactor* reactor, Observation* out, OffThreadWaits* waits)
{
    if (!waits->Keep(co_await AwaitUntil(
            reactor,
            "the reader to arm its first read",
            [out] { return out->arming.load(std::memory_order_acquire); },
            [out] { return Describe(*out); },
            ReactorWaitOptions { .context = {}, .bound = WaitHangGuard, .rest = std::chrono::milliseconds { 1 } })))
        co_return;

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

/// What one awaited operation came back as, published across the reactor and client
/// threads.
///
/// One struct rather than a fresh triple of atomics per operation: the probe case below
/// watches three, and three hand-written copies of *store the flag, store the code or
/// the count* is the copy-paste the project's own rule calls a defect -- they diverge,
/// and a case asserting on a field one of them forgot to publish passes for the wrong
/// reason.
struct OpOutcome
{
    std::atomic<bool> resolved { false }; ///< Set once the await has returned, whatever it returned.
    std::atomic<bool> hasValue { false }; ///< Whether it returned a count rather than an error.
    std::atomic<std::size_t> count { 0 }; ///< The count, when it returned one.
    std::atomic<int> errorCode { -1 };    ///< The `NetErrorCode`, when it returned one.
};

/// Publish what an awaited operation returned.
/// @param into   Where to publish it.
/// @param result What the await came back as.
void Record(OpOutcome& into, FastCache::IoResult const& result) noexcept
{
    into.hasValue.store(result.has_value(), std::memory_order_relaxed);
    if (result.has_value())
        into.count.store(*result, std::memory_order_relaxed);
    else
        into.errorCode.store(static_cast<int>(result.error().code), std::memory_order_relaxed);
    // Last, and with a release: every field above must be visible to a reader that sees
    // this one set.
    into.resolved.store(true, std::memory_order_release);
}

/// What the probe reader observed across TWO cancels.
struct ProbeObservation
{
    /// The accepted socket, for the canceller. A plain pointer, for the reason the
    /// case above gives: both coroutines run on the reactor thread and the reader
    /// outlives the canceller by construction.
    FastCache::ISocket* socket { nullptr };

    std::atomic<bool> arming { false }; ///< Set immediately before the probe that must park.

    /// Whether the FIRST probe had already resolved when the canceller ran. **The
    /// assertion that stops this case being vacuous**: a probe that resolved
    /// synchronously was never parked, and everything below would hold for a reason
    /// unrelated to cancellation.
    std::atomic<bool> firstResolvedAtCancel { true };

    /// Set by the reader immediately before the SECOND probe -- the one it arms while
    /// the first `CancelRead` is still on the stack.
    std::atomic<bool> secondArmed { false };

    /// What the canceller saw BETWEEN its two calls. `secondArmedAtCancel` is the
    /// inline-resumption premise itself: a transport that marshalled the first
    /// retirement instead would leave it false, and then the second call is not a
    /// double cancel at all but a first one, and the case must say so rather than pass.
    std::atomic<bool> secondArmedAtCancel { false };
    std::atomic<bool> secondResolvedAtCancel { true };

    OpOutcome first;  ///< The probe parked before any cancel.
    OpOutcome second; ///< The probe the FIRST cancel's own inline resumption armed.
    OpOutcome after;  ///< A real read afterwards -- the half that separates this from `Close()`.

    std::atomic<bool> finished { false }; ///< Set once the whole exchange is done.
};

/// What the probe exchange has reached, for a wait's account.
/// @param observed The reader's observation.
/// @return Its milestones, in words.
[[nodiscard]] std::string Describe(ProbeObservation const& observed)
{
    return std::format("reader armed {}, first probe resolved {}, second probe armed {}, second resolved {}, "
                       "exchange finished {}",
                       observed.arming.load(std::memory_order_acquire),
                       observed.first.resolved.load(std::memory_order_acquire),
                       observed.secondArmed.load(std::memory_order_acquire),
                       observed.second.resolved.load(std::memory_order_acquire),
                       observed.finished.load(std::memory_order_acquire));
}

/// Accept, park a readability PROBE, park a second one when that is retired, then read.
///
/// The second probe is the subject: it is armed from inside the first `CancelRead`,
/// because retiring a probe resumes this coroutine inline on every transport. By
/// POINTER, never by reference, for the reason the reader above gives.
/// @param reactor  Stopped once the exchange is complete, so `Run()` returns.
/// @param listener Bound listener to accept on.
/// @param out      Where the observation is published; must outlive the task.
FastCache::DetachedTask ProbeTwiceThenRead(FastCache::PlatformReactor* reactor,
                                           FastCache::IListener* listener,
                                           ProbeObservation* out)
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

    out->arming.store(true, std::memory_order_release);
    Record(out->first, co_await socket->WaitReadable());

    // **Armed from INSIDE the first `CancelRead`**, which is the whole mechanism: the
    // retirement completed this probe inline, so this statement runs before the
    // canceller's next one does. This is the operation the second cancel takes.
    out->secondArmed.store(true, std::memory_order_release);
    Record(out->second, co_await socket->WaitReadable());

    // And a real read, so the case can tell a cancel from a close. Legal only because
    // both retirements released the socket's single read slot (#663); under an
    // implementation that completed without detaching, this is the double-arm the slot
    // guard aborts on.
    std::array<std::byte, 8> buffer {};
    Record(out->after, co_await socket->Read(std::span<std::byte> { buffer }));

    out->finished.store(true, std::memory_order_release);
    socket->Close();
    reactor->Stop();
    co_return;
}

/// Cancel the reader's parked probe TWICE, in one reactor turn, from the reactor thread.
///
/// Twice with no suspension between, which is what makes the two calls the caller's
/// consecutive statements -- the arrangement #1233 is about. What the reader does
/// between them happens inside the first call, not between the two.
/// @param reactor The loop both tasks run on.
/// @param out     The reader's observation; must outlive the task.
/// @param waits   Keeps the wait's account for the case; must outlive the task.
FastCache::DetachedTask CancelParkedProbeTwice(FastCache::PlatformReactor* reactor,
                                               ProbeObservation* out,
                                               OffThreadWaits* waits)
{
    if (!waits->Keep(co_await AwaitUntil(
            reactor,
            "the reader to arm its first readability probe",
            [out] { return out->arming.load(std::memory_order_acquire); },
            [out] { return Describe(*out); },
            ReactorWaitOptions { .context = {}, .bound = WaitHangGuard, .rest = std::chrono::milliseconds { 1 } })))
        co_return;

    // Recorded BEFORE the cancel: the reader set `arming` and then suspended, and this
    // task only runs because it did, so a resolved probe here would mean it never parked.
    out->firstResolvedAtCancel.store(out->first.resolved.load(std::memory_order_acquire), std::memory_order_relaxed);

    out->socket->CancelRead();

    // Recorded BETWEEN the two calls, and these two are the case's premise rather than
    // its conclusion: the first call resumed the reader inline, and the reader armed
    // again and is parked. Neither is asserted here -- a `REQUIRE` on a reactor task is
    // not this case's thread -- so both are published and asserted on the main thread.
    out->secondArmedAtCancel.store(out->secondArmed.load(std::memory_order_acquire), std::memory_order_relaxed);
    out->secondResolvedAtCancel.store(out->second.resolved.load(std::memory_order_acquire), std::memory_order_relaxed);

    out->socket->CancelRead();
    co_return;
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

    // Declared before the reactor's tasks and the client, so it outlives both the coroutine
    // and the thread that wait through it.
    OffThreadWaits waits;
    Observation observed;
    ReadParkThenReadAgain(&reactor, listener.get(), &observed);
    CancelWhenParked(&reactor, &observed, &waits);

    // Recorded here and asserted on the main thread: a `REQUIRE` firing inside a
    // `jthread` body is `std::terminate`, not a failed case.
    std::atomic<bool> connected { false };

    std::jthread client { [port, &observed, &connected, &waits] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        connected.store(true, std::memory_order_release);

        // Nothing is sent until the first read has been cancelled, which is what makes
        // that read park rather than complete. Then two bytes, so the read issued after
        // the cancel has something to return.
        //
        // Twice the guard: this waits on the reactor task's own bounded wait for the
        // reader to arm, and two waits with one bound give up together and name the
        // symptom -- this one -- rather than the cause.
        if (!waits.WaitForFlag(
                "the parked first read to be cancelled",
                observed.firstResolved,
                [&observed] { return Describe(observed); },
                WaitOptions { .step = {}, .context = {}, .bound = 2 * WaitHangGuard, .rest = WaitRest }))
            return;

        std::array<std::byte, 2> const payload { std::byte { 'o' }, std::byte { 'k' } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 2> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        (void) waits.WaitForFlag("the exchange to finish", observed.finished, [&observed] { return Describe(observed); });
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    // First, while the accounts of any wait that ran out are still attached.
    CHECK(waits.AllReached());
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
    // Declared before the client, so it outlives the thread that waits through it.
    OffThreadWaits waits;

    std::jthread client { [port, &observed, &connected, &waits] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        connected.store(true, std::memory_order_release);

        // Written only once the server has cancelled and armed, so the read under test
        // is one that PARKED and was then satisfied -- not one that found bytes already
        // waiting and never suspended at all.
        if (!waits.WaitForFlag(
                "the reader to cancel twice and arm", observed.arming, [&observed] { return Describe(observed); }))
            return;

        std::array<std::byte, 3> const payload { std::byte { 'y' }, std::byte { 'e' }, std::byte { 's' } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 3> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        (void) waits.WaitForFlag("the exchange to finish", observed.finished, [&observed] { return Describe(observed); });
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    // First, while the accounts of any wait that ran out are still attached.
    CHECK(waits.AllReached());
    REQUIRE(connected.load(std::memory_order_acquire));
    REQUIRE(observed.finished.load(std::memory_order_acquire));

    INFO("read after two empty cancels: error code " << observed.secondErrorCode.load(std::memory_order_relaxed));
    CHECK(observed.secondHasValue.load(std::memory_order_relaxed));
    CHECK(observed.secondCount.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("A second CancelRead takes the probe the first one's resumption armed", "[net][socket][cancelread]")
{
    // **The case [#1233](https://github.com/LASTRADA-Software/fastcached/issues/1233)
    // asked for, and the one that settles which behaviour the declaration may claim.**
    //
    // `ISocket::CancelRead` used to say *idempotent* without qualification. It is not,
    // and the measurement is in this file's header: a retirement that completes its
    // victim INLINE resumes the reader before the call returns, the reader arms again,
    // and the second call retires the new operation. The header now says that instead --
    // and a sentence in a header is exactly what produced the wrong claim in the first
    // place, so this case is what holds the new one.
    //
    // **A PROBE rather than a real `Read`, and that choice is the finding.** #1233 was
    // filed believing the divergence was POSIX-versus-Windows. It is not: marshalling is
    // a property of the OPERATION. IOCP settles a real `Read` and retires a
    // `WaitReadable` probe inline, so over a probe all three transports behave the same
    // way -- one set of assertions covers every leg, with no `#if` and no per-platform
    // expectation. It is also the operation the verb exists for: production parks a
    // readability watch and cancels that, never a real read.
    //
    // **Five properties, and each fails under a different wrong implementation:**
    //
    //   1. the first probe was PARKED          -- otherwise the case is vacuous
    //   2. the first cancel resumed INLINE     -- the premise; a transport that
    //                                             marshalled would leave the reader
    //                                             un-rearmed, and the second call would
    //                                             not be a double cancel at all
    //   3. the first probe resolved CANCELLED  -- not EOF, which is what a peer leaving
    //                                             looks like
    //   4. the SECOND probe resolved CANCELLED -- the statement itself: the second call
    //                                             took an operation its caller never
    //                                             armed. An implementation that made
    //                                             `CancelRead` genuinely idempotent
    //                                             fails 2 and 4, which is correct -- the
    //                                             contract would have changed, and this
    //                                             case is what would say so.
    //   5. the socket still WORKS              -- an implementation spelled as `Close()`
    //                                             passes 1 to 3 and fails 4 and 5
    //
    // The inherited no-op fails 2 and 3: nothing is ever retired, the client's bounded
    // wait runs out, its close turns the still-parked probe into an EOF, and the case
    // reports that rather than hanging.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    // Declared before the reactor's tasks and the client, so it outlives both the
    // coroutine and the thread that wait through it.
    OffThreadWaits waits;
    ProbeObservation observed;
    ProbeTwiceThenRead(&reactor, listener.get(), &observed);
    CancelParkedProbeTwice(&reactor, &observed, &waits);

    // Recorded here and asserted on the main thread: a `REQUIRE` firing inside a
    // `jthread` body is `std::terminate`, not a failed case.
    std::atomic<bool> connected { false };

    std::jthread client { [port, &observed, &connected, &waits] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;
        connected.store(true, std::memory_order_release);

        // Nothing is sent until BOTH probes have been retired. A byte written earlier
        // would SATISFY a probe rather than let it be cancelled, and the case would be
        // measuring readability instead of cancellation -- green, and about nothing.
        //
        // Twice the guard, as the case above: this waits on the reactor task's own
        // bounded wait, and two waits with one bound give up together and name the
        // symptom rather than the cause.
        if (!waits.WaitForFlag(
                "both parked probes to be retired",
                observed.second.resolved,
                [&observed] { return Describe(observed); },
                WaitOptions { .step = {}, .context = {}, .bound = 2 * WaitHangGuard, .rest = WaitRest }))
            return;

        std::array<std::byte, 2> const payload { std::byte { 'o' }, std::byte { 'k' } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 2> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        (void) waits.WaitForFlag("the exchange to finish", observed.finished, [&observed] { return Describe(observed); });
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    // First, while the accounts of any wait that ran out are still attached.
    CHECK(waits.AllReached());
    REQUIRE(connected.load(std::memory_order_acquire));
    REQUIRE(observed.finished.load(std::memory_order_acquire));

    auto const cancelled = static_cast<int>(FastCache::NetErrorCode::Cancelled);

    // 1. The first probe really was parked when the canceller ran.
    CHECK_FALSE(observed.firstResolvedAtCancel.load(std::memory_order_relaxed));

    // 2. The premise: the first call resumed the reader INLINE, and the reader had armed
    //    a second probe and parked on it before the second call ran.
    CHECK(observed.secondArmedAtCancel.load(std::memory_order_relaxed));
    CHECK_FALSE(observed.secondResolvedAtCancel.load(std::memory_order_relaxed));

    // 3. The first probe resumed, as a cancellation rather than as EOF or readability.
    INFO("first probe: error code " << observed.first.errorCode.load(std::memory_order_relaxed)
                                    << " (Cancelled=" << cancelled << ")");
    REQUIRE(observed.first.resolved.load(std::memory_order_acquire));
    CHECK_FALSE(observed.first.hasValue.load(std::memory_order_relaxed));
    CHECK(observed.first.errorCode.load(std::memory_order_relaxed) == cancelled);

    // 4. **The statement.** The second call took the probe the first call's own
    //    resumption armed -- an operation its caller never asked to cancel.
    INFO("second probe: error code " << observed.second.errorCode.load(std::memory_order_relaxed)
                                     << " (Cancelled=" << cancelled << ")");
    REQUIRE(observed.second.resolved.load(std::memory_order_acquire));
    CHECK_FALSE(observed.second.hasValue.load(std::memory_order_relaxed));
    CHECK(observed.second.errorCode.load(std::memory_order_relaxed) == cancelled);

    // 5. And the socket was still a socket afterwards, which is what separates two
    //    cancels from a close.
    INFO("read after both cancels: error code " << observed.after.errorCode.load(std::memory_order_relaxed));
    CHECK(observed.after.hasValue.load(std::memory_order_relaxed));
    CHECK(observed.after.count.load(std::memory_order_relaxed) == 2);
}
