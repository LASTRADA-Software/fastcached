// SPDX-License-Identifier: Apache-2.0
//
// RESP over a REAL reactor socket.
//
// **Why this file exists at all.** Every other RESP case in this tree runs on
// `InMemoryTransport`, whose `WaitReadable` resolves synchronously and never parks.
// The pub/sub readable watcher is built entirely around parking on that call, so the
// question it exists to answer -- who owns the socket's single read-op slot, and when
// -- is exercised by nothing. That is not a gap in the coverage of a corner: it is the
// reason [#755](https://github.com/LASTRADA-Software/fastcached/issues/755) and
// [#710](https://github.com/LASTRADA-Software/fastcached/issues/710) could both sit in
// this code behind a green suite.
//
// So these cases drive `RedisRespHandler::Run` over a socket accepted on a real
// `PlatformReactor`, with a real client on a thread of its own. That is the only
// arrangement in which the watcher parks, and therefore the only one in which dropping
// it can be observed.

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/ResumeOn.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/CacheEngine.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/IConnector.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/PlatformListener.hpp>
#include <FastCache/Protocol/PubSubRegistry.hpp>
#include <FastCache/Protocol/RedisResp.hpp>
#include <FastCache/Protocol/SessionContext.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// Serve exactly one accepted connection with the RESP handler, then stop the reactor.
///
/// Everything by pointer or by value: this is a coroutine, so its frame outlives the
/// expression that created it and a reference parameter would bind to storage the
/// caller may already have released.
/// @param reactor Stopped once the session ends, so `Run()` returns.
/// @param listener Bound listener to accept on.
/// @param engine The cache the session serves.
/// @param session Session context, carrying the reactor and the pub/sub registry.
/// @param ended Set when the handler returned; must outlive the task.
/// @return The detached task.
DetachedTask ServeOneRespSession(
    PlatformReactor* reactor, IListener* listener, CacheEngine* engine, SessionContext session, std::atomic<bool>* ended)
{
    auto accepted = co_await listener->Accept();
    if (!accepted.has_value())
    {
        ended->store(true, std::memory_order_release);
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accepted);

    RedisRespHandler handler;
    co_await handler.Run(socket.get(), engine, /*primer*/ {}, session);

    socket->Close();

    // **Drain before stopping, the way a real server does by simply continuing to
    // run.** `Run`'s cleanup calls `ShutdownWatcher`, which wakes the readable
    // watcher through `IReactor::Submit` -- and a submitted handle on a reactor that
    // has already stopped is never resumed, so the watcher's coroutine frame leaks.
    // LeakSanitizer reported exactly that (1120 bytes in 6 allocations) the first
    // time this fixture ran under the ASan gate, and it is the FIXTURE's defect
    // rather than the session's: a daemon's reactor keeps running, so the same
    // resumption lands. One hop puts this continuation behind whatever cleanup
    // queued, so those run first.
    co_await ResumeOn { *reactor };

    ended->store(true, std::memory_order_release);
    reactor->Stop();
}

/// A client speaking RESP over a real socket, from a thread of its own.
///
/// Blocking on purpose: it runs on a `jthread`, never on the reactor, so `SyncRun`
/// over a blocking socket is the right driver -- every awaitable resolves inline and
/// nothing is left suspended, which is the one thing `SyncRun` refuses to read from.
class RespClient
{
  public:
    /// @param port The loopback port to dial.
    explicit RespClient(std::uint16_t port)
    {
        auto socket = SyncRun(_connector.Connect("127.0.0.1", port, DialOptions { .connectTimeout = 5s }));
        if (socket.has_value())
            _socket = std::move(*socket);
    }

    /// @return True when the connection was established.
    [[nodiscard]] bool Ok() const noexcept
    {
        return _socket != nullptr;
    }

    /// Send raw RESP bytes.
    /// @param wire What to send.
    /// @return True when the whole payload was accepted.
    [[nodiscard]] bool Send(std::string_view wire)
    {
        return SyncRun([](ISocket* s, std::string_view w) -> Task<bool> {
            auto const bytes = std::span<std::byte const> { reinterpret_cast<std::byte const*>(w.data()), w.size() };
            auto const r = co_await s->Write(bytes);
            co_return r.has_value() && *r == w.size();
        }(_socket.get(), wire));
    }

    /// Read whatever has arrived, once.
    /// @return The bytes as text; empty on EOF or error.
    [[nodiscard]] std::string ReadSome()
    {
        return SyncRun([](ISocket* s) -> Task<std::string> {
            std::vector<std::byte> chunk(4096);
            auto const r = co_await s->Read(std::span<std::byte> { chunk.data(), chunk.size() });
            if (!r.has_value() || *r == 0)
                co_return std::string {};
            co_return std::string { reinterpret_cast<char const*>(chunk.data()), *r };
        }(_socket.get()));
    }

    /// Read until @p marker has been seen or the peer stops talking.
    ///
    /// Bounded by attempts over reads that are themselves timeout-bounded, and it says
    /// what it waited for: a reply containing @p marker.
    /// @param marker Text the reply must contain.
    /// @return Everything read, whether or not the marker arrived.
    [[nodiscard]] std::string ReadUntil(std::string_view marker)
    {
        std::string out;
        for (int attempt = 0; attempt < 50; ++attempt)
        {
            auto const chunk = ReadSome();
            if (chunk.empty())
                break;
            out += chunk;
            if (out.contains(marker))
                break;
        }
        return out;
    }

    /// Close the client end.
    void Close()
    {
        if (_socket)
            _socket->Close();
    }

  private:
    BlockingConnector _connector;
    std::unique_ptr<ISocket> _socket;
};

/// Everything one real-socket RESP session needs, wired together.
struct RespOverSocket
{
    InMemoryLruStorage storage;
    SteadyClock clock;
    CacheEngine engine { storage, clock };
    PubSubRegistry pubsub;
    PlatformReactor reactor { clock };
    std::unique_ptr<PlatformListener> listener;
    std::atomic<bool> ended { false };

    /// Bind a per-run loopback port.
    ///
    /// `bind(0)`, so nothing on the machine can be holding it. Deliberately not the
    /// "below the ephemeral range" draw: that rule governs a fixture which draws a
    /// number and binds it later, and here the port arrives already bound.
    /// @return True when the listener bound.
    [[nodiscard]] bool Bind()
    {
        listener = PlatformListener::Bind(reactor, "127.0.0.1", 0);
        return listener != nullptr && listener->IsBound() && listener->BoundPort() != 0;
    }

    /// @return The port the kernel chose.
    [[nodiscard]] std::uint16_t Port() const
    {
        return listener->BoundPort();
    }

    /// @return A session carrying the reactor and the pub/sub registry, which is what
    ///         the readable watcher needs in order to exist at all.
    [[nodiscard]] SessionContext Session()
    {
        return SessionContext { .pubsub = &pubsub, .reactor = &reactor };
    }
};

} // namespace

TEST_CASE("RESP over a real socket: a command after UNSUBSCRIBE does not drop the watcher",
          "[protocol][resp][socket][pubsub]")
{
    // **#755.** `RearmReadable` runs BEFORE the command is dispatched, so on
    // `UNSUBSCRIBE` the watcher re-arms into `WaitReadable` and only then does the
    // subscription count reach zero. The loop stops entering the watcher block, calls
    // `ReadOneCommand`, and the `Read` underneath it claims the socket's single
    // read-op slot out from under a wait that is still parked.
    //
    // Since #725 that claim ASSERTS, so in a Debug build this is an abort rather than
    // a leak -- which is what makes it observable here at all. A release build leaks
    // the watcher's coroutine frame instead.
    //
    // Reachable only over a real socket: `InMemorySocket::WaitReadable` answers
    // synchronously, so the watcher never parks and the slot is never contended.
    RespOverSocket fix;
    if (!fix.Bind())
        SKIP("this host would not bind a loopback listener on any port");

    ServeOneRespSession(&fix.reactor, fix.listener.get(), &fix.engine, fix.Session(), &fix.ended);

    std::atomic<bool> survived { false };
    std::jthread client { [port = fix.Port(), &survived] {
        RespClient c { port };
        if (!c.Ok())
            return;
        // Enter subscribe mode: the watcher starts and parks on WaitReadable.
        if (!c.Send("*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n"))
            return;
        static_cast<void>(c.ReadUntil("subscribe"));

        // Leave it again. The watcher re-arms for this command and is then orphaned.
        if (!c.Send("*2\r\n$11\r\nUNSUBSCRIBE\r\n$2\r\nch\r\n"))
            return;
        static_cast<void>(c.ReadUntil("unsubscribe"));

        // **And now send NOTHING.** That is what makes this deterministic rather than
        // a race, and the first version of this case got it wrong: both `Read` and
        // `WaitReadable` try a SYNCHRONOUS `recv` first and return before they ever
        // reach `ClaimReadSlot`, so any byte in flight lets whichever call would have
        // claimed the slot take its fast path instead. Sending a further command made
        // the case pass against the unfixed code.
        //
        // With the wire quiet, the connection loop's `Read` parks (claiming the slot),
        // the watcher's queued re-arm then runs, and its `WaitReadable` finds nothing
        // to peek -- so it reaches the claim with the slot already taken.
        std::this_thread::sleep_for(300ms);

        // Closing is how the session ends once the defect is fixed; under the defect
        // the process has already aborted and this never runs.
        c.Close();
        survived.store(true, std::memory_order_relaxed);
    } };

    fix.reactor.Run();
    client.join();

    // Reaching here at all is the assertion: under the defect a Debug build aborts
    // inside `ClaimReadSlot` and the process never gets to any `CHECK`. The flag
    // separates "the sequence ran" from "the client could not connect", so a fixture
    // that never reached the interesting state cannot read as a pass.
    CHECK(survived.load(std::memory_order_relaxed));
}

TEST_CASE("RESP over a real socket: a subscriber is still woken by a published message", "[protocol][resp][socket][pubsub]")
{
    // **The control, and it is what stops the fix being "retire the watcher always".**
    // A connection that stays subscribed must still be woken by a delivery it did not
    // ask the socket for -- which is the watcher's entire purpose, and the half that a
    // retirement bug would silently remove. Both this and the case above would pass
    // against an implementation that never watched anything; only together do they
    // pin the behaviour.
    RespOverSocket fix;
    if (!fix.Bind())
        SKIP("this host would not bind a loopback listener on any port");

    ServeOneRespSession(&fix.reactor, fix.listener.get(), &fix.engine, fix.Session(), &fix.ended);

    std::atomic<bool> subscribed { false };
    std::atomic<bool> gotMessage { false };
    std::jthread client { [port = fix.Port(), &fix, &subscribed, &gotMessage] {
        RespClient c { port };
        if (!c.Ok())
            return;
        if (!c.Send("*2\r\n$9\r\nSUBSCRIBE\r\n$2\r\nch\r\n"))
            return;
        if (!c.ReadUntil("subscribe").contains("subscribe"))
            return;
        subscribed.store(true, std::memory_order_release);

        // Published from THIS thread through the registry, which is what a publisher
        // on another connection does. Nothing arrives on the subscriber's own socket,
        // so the only thing that can wake the session is the push arm.
        static_cast<void>(fix.pubsub.Publish("ch", "hello"));

        if (c.ReadUntil("hello").contains("hello"))
            gotMessage.store(true, std::memory_order_relaxed);
        c.Close();
    } };

    fix.reactor.Run();
    client.join();

    CHECK(subscribed.load(std::memory_order_acquire));
    CHECK(gotMessage.load(std::memory_order_relaxed));
}
