// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Transport/LingeringClose.hpp>
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/BlockingConnector.hpp>
#include <core/net/BlockingSocket.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/Sockets.hpp>
#include <core/net/testing/InMemorySocket.hpp>
#include <core/net/testing/TestLoop.hpp>
#include <core/platform/Clock.hpp>
#include <tests/HalfClose.hpp>
#include <tests/Unwrap.hpp>

// #1554. `CloseLingering` is what keeps a refusal a server wrote over a request it did not
// finish reading from being destroyed by the reset a bare close sends. These cases pin its
// exits -- the peer closed first, the peer finished while it listened, the read cap, the
// deadline on each kind of socket -- and, over a real loopback pair, the refusal arriving
// intact where a bare close loses it.

namespace
{

using namespace std::chrono_literals;

core::async::Task<bool> WriteString(core::net::ISocket* socket, std::string_view payload)
{
    auto const result = co_await socket->write(FastCache::AsBytes(payload));
    co_return result.has_value() && *result == payload.size();
}

/// One read, as it answered.
core::async::Task<core::net::IoResult> ReadOnce(core::net::ISocket* socket, std::span<std::byte> into)
{
    co_return co_await socket->read(into);
}

/// What the peer reads back: the text, and what ended it.
struct Received
{
    std::string text;           ///< Every byte read, in order.
    core::net::IoResult ending; ///< EOF as `0`, or the error that ended it.
};

/// Read until EOF or an error, and keep what ended it.
core::async::Task<Received> ReadToEnd(core::net::ISocket* socket)
{
    Received out { .text = {}, .ending = core::net::IoResult { std::size_t { 0 } } };
    std::array<std::byte, 256> chunk {};
    while (true)
    {
        auto const got = co_await socket->read(std::span<std::byte> { chunk });
        if (!got.has_value() || *got == 0)
        {
            out.ending = got;
            co_return out;
        }
        for (auto const b: std::span<std::byte const> { chunk }.first(*got))
            out.text.push_back(static_cast<char>(b));
    }
}

core::async::Task<core::net::AcceptResult> AcceptOne(core::net::IListener* listener)
{
    co_return co_await listener->accept();
}

constexpr auto Generous =
    FastCache::LingerBounds { .total = std::chrono::milliseconds { 1000 }, .maxBytes = std::size_t { 1 } << 20, .reads = 8 };

/// A request longer than the server reads of it before refusing, which is the whole shape.
constexpr std::string_view Request = "a request the server stopped reading after eight bytes, and refused";

/// What the server reads of `Request` before it refuses.
constexpr std::size_t ReadBeforeRefusing = 8;

/// The refusal.
constexpr std::string_view Refusal = "refused";

/// How the server end of a real pair closes after refusing.
enum class Closing : std::uint8_t
{
    Lingering, ///< Through `CloseLingering`, as production closes.
    Bare,      ///< `Close()` alone, which is the defect.
};

/// What the server end of a real refusal saw.
struct ServerSide
{
    std::atomic<bool> done { false };                ///< The server finished, however it went.
    std::optional<FastCache::LingerOutcome> outcome; ///< How the linger ended, when it lingered.
};

/// The server end of a real refusal, on the reactor: read a little of the request, refuse,
/// close. By pointer throughout, because a coroutine's frame outlives its caller's locals.
/// @param reactor Stopped once the server has closed.
/// @param listener Where the client dials.
/// @param closing How the server closes after refusing.
/// @param out What the server saw; must outlive the task.
core::async::DetachedTask RefuseOnReactor(core::net::PlatformLoop* reactor,
                                          core::net::IListener* listener,
                                          Closing closing,
                                          ServerSide* out)
{
    auto accepted = co_await listener->accept();
    if (accepted.has_value())
    {
        auto socket = std::move(*accepted);
        std::array<std::byte, ReadBeforeRefusing> head {};
        (void) co_await socket->read(std::span<std::byte> { head });
        (void) co_await socket->write(FastCache::AsBytes(Refusal));
        if (closing == Closing::Lingering)
            out->outcome = co_await FastCache::CloseLingering(socket.get(), reactor, Generous);
        else
            socket->close();
    }
    out->done.store(true, std::memory_order_release);
    reactor->stop();
}

/// Refuse a request over a real loopback pair, and return what the client read.
///
/// The server is a reactor socket, as the daemon's connections are; the client is a
/// blocking one on its own thread, which writes the whole request at once -- so the part
/// the server never reads is in its receive buffer when it closes -- then reads to the end,
/// then closes, as a well-behaved client does on seeing the server's FIN.
/// @param closing How the server closes after refusing.
/// @param server What the server saw.
/// @return What the client read.
[[nodiscard]] Received RefuseOverLoopback(Closing closing, ServerSide& server)
{
    core::platform::SteadyClock clock;
    core::net::PlatformLoop reactor { clock };
    auto listened = core::net::listen(reactor, core::net::ListenOptions { .host = "127.0.0.1", .port = 0 });
    REQUIRE(listened.has_value());
    auto const listener = std::move(*listened);
    auto const port = listener->boundPort();

    RefuseOnReactor(&reactor, listener.get(), closing, &server);

    Received received { .text = {}, .ending = core::net::IoResult { std::size_t { 0 } } };
    std::jthread client { [port, &received] {
        core::net::BlockingConnector connector;
        auto socket = core::async::syncRun(
            connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (!socket.has_value())
        {
            received.ending = std::unexpected(socket.error());
            return;
        }
        // A read nothing answers is bounded rather than hung.
        (*socket)->setReceiveDeadline(std::chrono::milliseconds { 5000 });
        (void) core::async::syncRun(WriteString(socket->get(), Request));
        received = core::async::syncRun(ReadToEnd(socket->get()));
        (*socket)->close();
    } };

    reactor.run();
    client.join();
    return received;
}

} // namespace

TEST_CASE("CloseLingering over a peer that closed first reads once and closes", "[net][linger]")
{
    // The ordinary goodbye, and the property that keeps it one: a peer that has finished
    // costs a single read, which answers EOF at once. A linger that waited here would turn
    // every ending into a delay.
    auto pair = core::net::testing::InMemorySocketPair::create();
    pair.client->close();

    auto const outcome = core::async::syncRun(FastCache::CloseLingering(pair.server.get(), nullptr, Generous));

    CHECK(outcome.end == FastCache::LingerEnd::PeerFinished);
    CHECK(outcome.reads == 1);
    CHECK(pair.server->isClosed());
}

TEST_CASE("CloseLingering leaves an already closed socket closed, and reads nothing", "[net][linger]")
{
    auto pair = core::net::testing::InMemorySocketPair::create();
    pair.server->close();

    auto const outcome = core::async::syncRun(FastCache::CloseLingering(pair.server.get(), nullptr, Generous));

    CHECK(outcome.end == FastCache::LingerEnd::AlreadyClosed);
    CHECK(outcome.reads == 0);
    CHECK(pair.server->isClosed());
}

TEST_CASE("CloseLingering drains a refused request and closes on the peer's EOF", "[net][linger]")
{
    // The reply is written while the peer's request is still unread -- a refusal of a
    // request the server did not finish reading. The peer has finished sending, so the
    // linger reads the rest, meets the EOF, and the peer reads the reply and then EOF.
    auto pair = core::net::testing::InMemorySocketPair::create();
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), Request)));
    REQUIRE(FastCache::Testing::ShutdownWrite(*pair.client).has_value());
    REQUIRE(core::async::syncRun(WriteString(pair.server.get(), Refusal)));

    auto const outcome = core::async::syncRun(FastCache::CloseLingering(pair.server.get(), nullptr, Generous));
    CHECK(outcome.end == FastCache::LingerEnd::PeerFinished);
    CHECK(outcome.reads == 2);

    auto const received = core::async::syncRun(ReadToEnd(pair.client.get()));
    CHECK(received.text == Refusal);
    REQUIRE(received.ending.has_value());
    CHECK(*received.ending == 0);
}

TEST_CASE("CloseLingering stops at whichever cap a peer meets first", "[net][linger]")
{
    // Bounded best effort, and the bound is stated rather than hidden: the linger is never a
    // promise to read an unbounded upload in order to refuse it. Each cap is asserted with
    // the other set well clear of it, so the case says WHICH one stopped the drain.
    auto pair = core::net::testing::InMemorySocketPair::create();
    std::string const large(64 * 1024, 'u');
    REQUIRE(core::async::syncRun(WriteString(pair.client.get(), large)));
    REQUIRE(FastCache::Testing::ShutdownWrite(*pair.client).has_value());

    SECTION("the reads")
    {
        auto const outcome = core::async::syncRun(FastCache::CloseLingering(
            pair.server.get(),
            nullptr,
            FastCache::LingerBounds { .total = 1000ms, .maxBytes = large.size() * 2, .reads = 1 }));
        CHECK(outcome.end == FastCache::LingerEnd::ReadCap);
        CHECK(outcome.reads == 1);
    }

    SECTION("the bytes")
    {
        auto const outcome = core::async::syncRun(FastCache::CloseLingering(
            pair.server.get(), nullptr, FastCache::LingerBounds { .total = 1000ms, .maxBytes = 1, .reads = 8 }));
        CHECK(outcome.end == FastCache::LingerEnd::ByteCap);
        CHECK(outcome.reads == 1);
    }

    CHECK(pair.server->isClosed());
}

TEST_CASE("CloseLingering on a reactor stops listening to a silent peer at its deadline", "[net][linger]")
{
    // A peer that neither sends nor closes: the linger parks, and only the deadline ends it.
    // Asserted parked BEFORE the clock moves, or a linger that never waited at all would pass.
    core::platform::ManualClock clock;
    core::net::testing::TestLoop reactor { clock };
    auto pair = core::net::testing::InMemorySocketPair::create();

    auto linger = FastCache::CloseLingering(
        pair.server.get(),
        &reactor,
        FastCache::LingerBounds { .total = std::chrono::milliseconds { 100 }, .maxBytes = 4096, .reads = 4 });
    auto handle = linger.handle();
    handle.resume();
    REQUIRE_FALSE(linger.done());
    CHECK_FALSE(pair.server->isClosed());

    clock.advance(std::chrono::milliseconds { 99 });
    std::ignore = reactor.drain();
    CHECK_FALSE(linger.done());

    clock.advance(std::chrono::milliseconds { 1 });
    std::ignore = reactor.drain();
    REQUIRE(linger.done());
    auto const outcome = linger.result();
    CHECK(outcome.end == FastCache::LingerEnd::Expired);
    CHECK(outcome.reads == 1);
    CHECK(pair.server->isClosed());
}

TEST_CASE("CloseLingering on a blocking socket stops listening to a silent peer", "[net][linger][socket]")
{
    // The admin surface's sockets BLOCK, so no reactor deadline reaches them: each read
    // carries its share of the total. A real pair, because a receive deadline is a property
    // of the kernel's socket and the in-process one has no clock.
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener);
    auto const port = listener->boundPort();
    REQUIRE(port != 0);

    core::net::BlockingConnector connector;
    auto client =
        core::async::syncRun(connector.connect("127.0.0.1", port, core::net::DialOptions { .connectTimeout = 5s }));
    REQUIRE(client.has_value());
    auto accepted = core::async::syncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());
    auto& server = *accepted;

    // The client stays open and silent throughout, so only the deadline can end the linger,
    // and a read with none would block this thread for good.
    auto const outcome = core::async::syncRun(FastCache::CloseLingering(
        server.get(), nullptr, FastCache::LingerBounds { .total = 200ms, .maxBytes = 4096, .reads = 2 }));
    CHECK(outcome.end == FastCache::LingerEnd::Expired);
    CHECK(outcome.reads == 1);
    CHECK(server->isClosed());

    // And the close was the FIN the half-close promised: nothing of the client's was unread.
    std::array<std::byte, 8> buffer {};
    auto const got = core::async::syncRun(ReadOnce(client->get(), std::span<std::byte> { buffer }));
    REQUIRE(got.has_value());
    CHECK(*got == 0);
}

TEST_CASE("A refusal written over an unread request reaches a real client intact only when the close lingers",
          "[net][linger][socket]")
{
    // The defect, on the wire rather than in a model of it. The server reads eight bytes of
    // the request, refuses, and closes with the rest still in its receive buffer. Measured
    // on loopback (#1553): the bare close is a reset, and Windows drops the refusal the
    // client had not read yet while Linux and macOS deliver it and then report the reset.
    // So the refusal arriving is not enough -- it must arrive AND end in the EOF of a close
    // that was a FIN, which is what fails on every platform when the close is bare.
    SECTION("lingering: the refusal, then EOF")
    {
        ServerSide server;
        auto const received = RefuseOverLoopback(Closing::Lingering, server);
        REQUIRE(server.done.load(std::memory_order_acquire));
        CHECK(received.text == Refusal);
        REQUIRE(received.ending.has_value());
        CHECK(*received.ending == 0);
        REQUIRE(server.outcome.has_value());
        CHECK(FastCache::Testing::Unwrap(server.outcome).end == FastCache::LingerEnd::PeerFinished);
    }

    SECTION("control, a bare close: a reset, with the refusal lost on Windows")
    {
        ServerSide server;
        auto const received = RefuseOverLoopback(Closing::Bare, server);
        REQUIRE(server.done.load(std::memory_order_acquire));
        INFO("the client read \"" << received.text << "\"");
        CHECK_FALSE(received.ending.has_value());
    }
}
