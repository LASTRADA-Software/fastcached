// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/PlatformListener.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <thread>
#include <vector>

/// What `ISocket::WaitReadable` reports, on a REAL socket, on this platform.
///
/// **The primitive had no test at all** -- zero matches for `WaitReadable` across
/// every file in `src/FastCache/Net/*_test.cpp` before this one -- which is how three
/// implementations came to disagree about the value they resolve with and nothing
/// noticed. `EpollSocket` and `KqueueSocket` answer `1` whatever they peeked;
/// `IocpSocket` posts a zero-byte `WSARecv` and completes with the byte count the
/// kernel wrote, which for a zero-byte receive is `0` -- so the same call reports the
/// opposite number on Windows from the number it reports on Linux, for the same event.
///
/// The interface called the count "advisory", which was true and is what let the
/// disagreement stand. This file is what makes it mean something:
///
///   - `0`  the peer has closed its write side; a `Read` here returns EOF.
///   - `>0` bytes are pending; a `Read` here returns some of them.
///
/// Deliberately cross-platform rather than three copies under three `#if`s: the whole
/// defect is that the platforms disagreed, and a per-platform test is exactly the
/// shape that cannot see that. It drives `PlatformReactor` / `PlatformListener`, so
/// each CI leg exercises its own implementation against these same assertions.
namespace
{

/// What one `WaitReadable` resolved to, published across the two threads.
struct Observation
{
    /// Set once the await has returned. Read by the client thread BEFORE it acts, so a
    /// case can prove the wait was actually PARKED rather than resolved on the spot --
    /// see the parked cases, where a synchronous resolution would silently test the
    /// other path and still pass.
    std::atomic<bool> resolved { false };

    /// Set immediately before the await. The client waits for it, which is what makes
    /// "the server is parked" the overwhelmingly likely state when the client acts.
    std::atomic<bool> arming { false };

    std::atomic<bool> hasValue { false };
    std::atomic<std::size_t> count { 0 };

    /// Bytes a follow-up `Read` returned, for the case that asserts the peek consumed
    /// nothing.
    std::atomic<std::size_t> readBack { 0 };

    /// Whether the observer should write a reply once it has seen the peer's EOF.
    ///
    /// Plain, not atomic: set before the task is started and never touched again.
    bool replyAfterEof { false };

    /// Whether that reply was accepted by the socket.
    std::atomic<bool> replySent { false };
};

/// Accept one connection, wait for readability, and publish what came back.
///
/// Everything by POINTER rather than by reference, because this is a coroutine: its
/// frame outlives the expression that created it, so a reference parameter binds to
/// storage the caller may already have released. clang-tidy enforces it and is right
/// to; the same rule every coroutine in this tree carries.
/// @param reactor Stopped once the observation is complete, so `Run()` returns.
/// @param listener Bound listener to accept on.
/// @param out Where the observation is published; must outlive the task.
/// @param readAfter Whether to issue a real `Read` afterwards, to prove the peek
///        consumed nothing.
FastCache::DetachedTask ObserveOne(FastCache::PlatformReactor* reactor,
                                   FastCache::IListener* listener,
                                   Observation* out,
                                   bool readAfter)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        out->resolved.store(true, std::memory_order_release);
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accepted);

    out->arming.store(true, std::memory_order_release);
    auto const readable = co_await socket->WaitReadable();

    out->hasValue.store(readable.has_value(), std::memory_order_relaxed);
    if (readable.has_value())
        out->count.store(*readable, std::memory_order_relaxed);

    if (readAfter && readable.has_value())
    {
        std::array<std::byte, 8> buf {};
        if (auto const got = co_await socket->Read(std::span<std::byte> { buf }); got.has_value())
            out->readBack.store(*got, std::memory_order_relaxed);
    }

    // Answering AFTER the peer's EOF, which is the wire's rule made executable: a
    // half-close says the peer has finished sending, not that it has gone, so a reply
    // it is already owed must still reach it.
    if (out->replyAfterEof)
    {
        std::array<std::byte, 3> const owed { std::byte { 'o' }, std::byte { 'w' }, std::byte { 'e' } };
        if ((co_await socket->Write(std::span<std::byte const> { owed })).has_value())
            out->replySent.store(true, std::memory_order_relaxed);
    }

    out->resolved.store(true, std::memory_order_release);
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

TEST_CASE("WaitReadable reports zero when a parked peer closes gracefully", "[net][socket][waitreadable]")
{
    // **The case the whole thing exists for**, and the one no in-tree test could reach:
    // a peer that goes away the ordinary way sends FIN, which is readability rather
    // than an error. A caller told only "readable" cannot tell that from a pipelined
    // request, so it either abandons a live peer or waits forever on a dead one.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    Observation observed;
    ObserveOne(&reactor, listener.get(), &observed, /*readAfter*/ false);

    std::jthread client { [port, &observed] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;

        // Waited for: the server to reach its await. **Then asserted still unresolved**
        // -- without that, a `WaitReadable` that answered synchronously would test the
        // other path entirely and this case would pass having never parked anything.
        (void) WaitForFlag(observed.arming);
        REQUIRE_FALSE(observed.resolved.load(std::memory_order_acquire));

        // A full, graceful close: FIN, not RST.
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    REQUIRE(observed.resolved.load(std::memory_order_acquire));
    REQUIRE(observed.hasValue.load(std::memory_order_relaxed)); // EOF is not an error.
    CHECK(observed.count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("WaitReadable reports non-zero for pending data and consumes none of it", "[net][socket][waitreadable]")
{
    // The other half, and it is what stops the fix being "report 0 always": a caller
    // acting on `0` must be able to trust that `>0` means there is something to read.
    // The follow-up `Read` is the part that matters -- a probe that CONSUMED the byte
    // to learn about it would satisfy the count and break every caller.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    Observation observed;
    ObserveOne(&reactor, listener.get(), &observed, /*readAfter*/ true);

    std::jthread client { [port, &observed] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;

        (void) WaitForFlag(observed.arming);
        REQUIRE_FALSE(observed.resolved.load(std::memory_order_acquire));

        std::array<std::byte, 1> const payload { std::byte { 0x7A } };
        (void) FastCache::SyncRun([](FastCache::ISocket* s, std::array<std::byte, 1> p) -> FastCache::Task<bool> {
            co_return (co_await s->Write(std::span<std::byte const> { p })).has_value();
        }((*socket).get(), payload));

        // Held open until the server has finished, so the close cannot race the
        // observation and turn this into the EOF case.
        (void) WaitForFlag(observed.resolved);
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    REQUIRE(observed.resolved.load(std::memory_order_acquire));
    REQUIRE(observed.hasValue.load(std::memory_order_relaxed));
    CHECK(observed.count.load(std::memory_order_relaxed) > 0);

    // Nothing was consumed by the probe: the byte is still there for the real read.
    CHECK(observed.readBack.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("WaitReadable reports zero when EOF is already pending before it is called", "[net][socket][waitreadable]")
{
    // The synchronous arm. `EpollSocket` and `KqueueSocket` answer this one out of a
    // `recv(MSG_PEEK)` without parking at all, and `IocpSocket` still posts its
    // zero-byte receive -- so the two paths are different code and this is the only
    // case that pins the first. **Both must give the same answer**, which is the
    // property that stops a caller's behaviour depending on how busy the peer was.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    // The client connects and closes immediately, so the FIN is very likely to have
    // arrived before the server asks. If it has not, this case degenerates into the
    // parked one above and still asserts the right answer -- which is why the
    // assertion is on the VALUE and not on which path produced it.
    std::atomic<bool> closed { false };
    std::jthread client { [port, &closed] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (socket.has_value())
            (*socket)->Close();
        closed.store(true, std::memory_order_release);
    } };

    Observation observed;
    ObserveOne(&reactor, listener.get(), &observed, /*readAfter*/ false);
    reactor.Run();
    client.join();

    REQUIRE(closed.load(std::memory_order_acquire));
    REQUIRE(observed.resolved.load(std::memory_order_acquire));
    REQUIRE(observed.hasValue.load(std::memory_order_relaxed));
    CHECK(observed.count.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("A half-closed peer still receives what it is owed", "[net][socket][waitreadable][halfclose]")
{
    // **The wire's rule, executable.** `ISocket::ShutdownWrite` exists so a peer can
    // say "I have finished sending" without saying "I have gone", and the rule
    // (`.agent/rules/wire-and-protocol.md`) is that a server answers what it already
    // owes such a peer. This is that sentence as a test: the server sees EOF -- count
    // `0`, the #677 contract -- and its reply still arrives.
    //
    // It is also the first production use of `ShutdownWrite` in this tree. Until now
    // only `InMemorySocket` had one and every caller was a test, which is exactly why
    // the question could not be settled from inside the repository.
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    auto const port = listener->BoundPort();
    REQUIRE(port != 0);

    Observation observed;
    observed.replyAfterEof = true;
    ObserveOne(&reactor, listener.get(), &observed, /*readAfter*/ false);

    // Recorded in the client thread, asserted on the main one. A failing Catch2
    // assertion throws, and thrown out of a `jthread` body that is `std::terminate`
    // -- measured: staging the half-close as a full `Close()` ended the process with
    // exit 3 instead of reporting a failed case. The reading still has to happen in
    // the thread that owns the socket, so what crosses back is the observation.
    std::atomic<bool> openAfterHalfClose { false };

    // The same marshalling, for the same reason. This one reads "the observer had
    // not resolved before we half-closed", which is what makes the reply below an
    // answer to the EOF rather than something already in flight -- and asserting it
    // HERE, on the client thread, is the exact hazard the paragraph above records:
    // a `REQUIRE` that fires inside a `jthread` body terminates the process instead
    // of failing the case. The remedy had been applied three lines below it and not
    // to it.
    std::atomic<bool> unresolvedBeforeHalfClose { false };

    std::vector<std::byte> received;
    std::jthread client { [port, &observed, &received, &openAfterHalfClose, &unresolvedBeforeHalfClose] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
            return;

        (void) WaitForFlag(observed.arming);
        unresolvedBeforeHalfClose.store(!observed.resolved.load(std::memory_order_acquire), std::memory_order_relaxed);

        // **Half-close, not close.** The read half stays open, which is the whole
        // point: a `Close()` here would make the assertion below unreachable.
        (*socket)->ShutdownWrite();
        openAfterHalfClose.store(!(*socket)->IsClosed(), std::memory_order_relaxed);

        // **Bounded, and it is what keeps a missing fix a FAILURE rather than a
        // hang.** With no `ShutdownWrite` on the transport the interface default is a
        // no-op, so the server never sees EOF, never resolves, and never stops its
        // reactor -- and a test that hangs reports nothing at all. Waiting here and
        // closing anyway lets the server unpark, the reactor return, and the
        // assertions below say what was actually wrong.
        if (!WaitForFlag(observed.resolved))
        {
            (*socket)->Close();
            return;
        }

        received = FastCache::SyncRun([](FastCache::ISocket* s) -> FastCache::Task<std::vector<std::byte>> {
            std::array<std::byte, 8> buf {};
            auto const got = co_await s->Read(std::span<std::byte> { buf });
            if (!got.has_value() || *got == 0)
                co_return std::vector<std::byte> {};
            co_return std::vector<std::byte> { buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(*got) };
        }((*socket).get()));
        (*socket)->Close();
    } };

    reactor.Run();
    client.join();

    REQUIRE(observed.resolved.load(std::memory_order_acquire));
    REQUIRE(observed.hasValue.load(std::memory_order_relaxed));

    // The observer was still parked when the half-close happened, so the reply below
    // answers the EOF and is not something that was already on its way.
    CHECK(unresolvedBeforeHalfClose.load(std::memory_order_relaxed));

    // A half-close is not a close: the client's socket stayed open to read with.
    CHECK(openAfterHalfClose.load(std::memory_order_relaxed));

    // The server saw "finished sending", spelled as the #677 count.
    CHECK(observed.count.load(std::memory_order_relaxed) == 0);

    // And the peer, which had closed only its write half, still got its answer.
    CHECK(observed.replySent.load(std::memory_order_relaxed));
    CHECK(received.size() == 3);
}
