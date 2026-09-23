// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Async/Task.hpp>
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/PlatformListener.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <tests/BoundedWait.hpp>
#include <tests/Unwrap.hpp>

// #1556. A socket `IocpListener` accepted did not have its context updated
// (`SO_UPDATE_ACCEPT_CONTEXT`), so `shutdown` failed on it with `WSAENOTCONN` and
// `ShutdownWrite` was a silent no-op on every accepted Windows socket: no FIN reached the
// peer, and writes went on succeeding. Cross-platform on purpose -- the defect was one
// platform's, and a test on that platform alone is the shape that cannot see the platforms
// disagreeing.

namespace
{

using namespace std::chrono_literals;

/// What the accepted end saw, and when the dialling end is done with it.
struct Exchange
{
    std::atomic<bool> clientRead { false };        ///< The client has read after the half-close.
    std::atomic<bool> serverDone { false };        ///< The server has closed, however it went.
    std::optional<FastCache::IoResult> writeAfter; ///< The server's write after its own half-close.
    std::string account;                           ///< Why the server's wait ran out, when it did.
};

/// The accepted end, on the reactor: half-close, write once more, then hold the socket open
/// until the client has read -- a close before that would send the FIN the half-close is
/// being tested for, and the case would pass on the defect.
/// @param reactor Stopped once the server has closed.
/// @param listener Where the client dials.
/// @param out Shared with the client's thread; must outlive the task.
FastCache::DetachedTask HalfCloseAccepted(FastCache::PlatformReactor* reactor, FastCache::IListener* listener, Exchange* out)
{
    auto accepted = co_await listener->Accept();
    if (accepted.has_value())
    {
        auto socket = std::move(*accepted);
        socket->shutdownWrite();
        std::array<std::byte, 1> const one { std::byte { 0x41 } };
        out->writeAfter = co_await socket->write(std::span<std::byte const> { one });
        auto const read = co_await FastCache::Testing::AwaitUntil(
            reactor,
            "the client to read after the half-close",
            [out] { return out->clientRead.load(std::memory_order_acquire); },
            [out] { return std::format("client read {}", out->clientRead.load(std::memory_order_acquire)); },
            FastCache::Testing::ReactorWaitOptions {
                .context = {}, .bound = FastCache::Testing::WaitHangGuard, .rest = std::chrono::milliseconds { 1 } });
        if (!read.reached)
            out->account = read.account;
        socket->close();
    }
    out->serverDone.store(true, std::memory_order_release);
    reactor->stop();
}

FastCache::Task<FastCache::IoResult> ReadOnce(FastCache::ISocket* socket, std::span<std::byte> into)
{
    co_return co_await socket->read(into);
}

} // namespace

TEST_CASE("An accepted socket's ShutdownWrite reaches its peer as EOF, and its own writes then fail",
          "[net][socket][halfclose]")
{
    FastCache::SteadyClock clock;
    FastCache::PlatformReactor reactor { clock };
    auto listener = FastCache::PlatformListener::Bind(reactor, "127.0.0.1", 0);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());
    auto const port = listener->BoundPort();

    Exchange exchange;
    HalfCloseAccepted(&reactor, listener.get(), &exchange);

    // Recorded here, asserted after the join: a `REQUIRE` thrown out of a `jthread` body is
    // `std::terminate`, not a failed case.
    std::optional<FastCache::IoResult> clientRead;
    std::jthread client { [port, &exchange, &clientRead] {
        FastCache::BlockingConnector connector;
        auto socket = FastCache::SyncRun(
            connector.connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = std::chrono::seconds { 5 } }));
        if (socket.has_value())
        {
            // Bounded: on the defect nothing ever arrives, and the read would otherwise block
            // until the server's close -- whose FIN would then pass for the half-close's.
            (*socket)->setReceiveDeadline(std::chrono::milliseconds { 2000 });
            std::array<std::byte, 8> buffer {};
            clientRead = FastCache::SyncRun(ReadOnce(socket->get(), std::span<std::byte> { buffer }));
        }
        exchange.clientRead.store(true, std::memory_order_release);
    } };

    reactor.run();
    client.join();

    CHECK(exchange.account.empty());
    REQUIRE(exchange.serverDone.load(std::memory_order_acquire));
    REQUIRE(clientRead.has_value());
    auto const& read = FastCache::Testing::Unwrap(clientRead);
    INFO("the client's read: " << (read.has_value() ? std::format("{} bytes", *read) : read.error().ToString()));
    REQUIRE(read.has_value());
    CHECK(*read == 0);

    REQUIRE(exchange.writeAfter.has_value());
    CHECK_FALSE(FastCache::Testing::Unwrap(exchange.writeAfter).has_value());
}
