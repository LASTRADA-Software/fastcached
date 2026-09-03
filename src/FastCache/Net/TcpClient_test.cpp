// SPDX-License-Identifier: Apache-2.0
//
// The one TCP client. What matters here is the partial-transfer loops -- the
// rule "keep going until the buffer is done, and say whether it finished" --
// because that is what three separate copies of this code each had to get right
// and what a caller silently mis-handles when it is wrong.
//
// The loops are driven against a scripted ISocket rather than a live peer: a
// real loopback socket transfers small buffers in one call, so the partial path
// -- the only interesting one -- would never execute.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/ISocket.hpp>
#include <FastCache/Net/TcpClient.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// A socket whose every call is scripted, so a partial transfer is reproducible.
///
/// Real sockets decide how much of a buffer they take, which makes the case that
/// matters -- a write or read that completes in pieces -- something a test can
/// only wait for and hope. Here it is stated.
class ScriptedSocket final: public ISocket
{
  public:
    /// @param chunks How many bytes each successive call transfers. A zero means
    ///        EOF for a read and a stalled write; running off the end of the
    ///        script is an error, which is how "it asked more times than it
    ///        should have" is caught rather than silently tolerated.
    explicit ScriptedSocket(std::deque<std::size_t> chunks) noexcept:
        _chunks { std::move(chunks) }
    {
    }

    [[nodiscard]] IoAwaitable Read(std::span<std::byte> buffer) override
    {
        auto const n = Next();
        if (!n.has_value())
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "script exhausted" }) };
        auto const take = std::min(*n, buffer.size());
        std::ranges::fill(buffer.first(take), std::byte { 0x5A });
        return IoAwaitable { IoResult { take } };
    }

    [[nodiscard]] IoAwaitable Write(std::span<std::byte const> buffer) override
    {
        auto const n = Next();
        if (!n.has_value())
            return IoAwaitable { std::unexpected(
                NetError { .code = NetErrorCode::ConnReset, .systemCode = 0, .context = "script exhausted" }) };
        auto const take = std::min(*n, buffer.size());
        _written.insert(_written.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(take));
        return IoAwaitable { IoResult { take } };
    }

    [[nodiscard]] IoAwaitable WriteVectored(std::span<std::span<std::byte const> const> /*segments*/,
                                            std::shared_ptr<void const> /*keepAlive*/ = {}) override
    {
        return IoAwaitable { IoResult { 0 } };
    }

    void Close() noexcept override
    {
        _closed = true;
    }
    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return _closed;
    }
    [[nodiscard]] std::string PeerAddress() const override
    {
        return "scripted";
    }

    /// Everything Write() accepted, in order.
    [[nodiscard]] std::vector<std::byte> const& Written() const noexcept
    {
        return _written;
    }

    /// How many calls the script still has left.
    [[nodiscard]] std::size_t Remaining() const noexcept
    {
        return _chunks.size();
    }

  private:
    [[nodiscard]] std::optional<std::size_t> Next()
    {
        if (_chunks.empty())
            return std::nullopt;
        auto const n = _chunks.front();
        _chunks.pop_front();
        return n;
    }

    std::deque<std::size_t> _chunks;
    std::vector<std::byte> _written;
    bool _closed { false };
};

/// A payload of `count` distinct-ish bytes.
[[nodiscard]] std::vector<std::byte> Payload(std::size_t count)
{
    std::vector<std::byte> bytes(count);
    for (std::size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<std::byte>(i & 0xFFU);
    return bytes;
}

/// Await one accept. A blocking listener resolves it synchronously, so `SyncRun`
/// is the right driver -- the task is never left suspended, which is the one
/// thing `SyncRun` refuses to read from. A pointer because a coroutine parameter
/// must not be a reference.
/// @param listener The bound listener.
/// @return The accepted socket, or the accept error.
[[nodiscard]] Task<AcceptResult> AcceptOne(BlockingListener* listener)
{
    co_return co_await listener->Accept();
}

} // namespace

TEST_CASE("SendAll keeps writing until the whole buffer is gone", "[net][tcpclient]")
{
    // The defect this loop exists to prevent is a *silent* one: a client that
    // treats one short write as success sends a truncated frame, and the peer
    // then blocks waiting for the rest of a length it was promised.
    auto const payload = Payload(10);
    ScriptedSocket socket { { 3, 3, 4 } };

    CHECK(SyncRun(SendAll(&socket, std::span<std::byte const> { payload })));
    CHECK(socket.Written() == payload);
    CHECK(socket.Remaining() == 0);
}

TEST_CASE("SendAll reports a write that fails part-way", "[net][tcpclient]")
{
    auto const payload = Payload(10);
    ScriptedSocket socket { { 4 } }; // then the script runs out: an error

    CHECK_FALSE(SyncRun(SendAll(&socket, std::span<std::byte const> { payload })));
}

TEST_CASE("SendAll treats a zero-length write as failure rather than looping", "[net][tcpclient]")
{
    // Without this the loop spins forever against a socket that accepts nothing
    // but reports no error -- a hang rather than a failed build, which is the
    // worse of the two outcomes.
    auto const payload = Payload(4);
    ScriptedSocket socket { { 0, 0, 0 } };

    CHECK_FALSE(SyncRun(SendAll(&socket, std::span<std::byte const> { payload })));
    CHECK(socket.Remaining() == 2); // it gave up after the first, not after all three
}

TEST_CASE("SendAll of nothing succeeds without touching the socket", "[net][tcpclient]")
{
    ScriptedSocket socket { {} };
    CHECK(SyncRun(SendAll(&socket, std::span<std::byte const> {})));
}

TEST_CASE("RecvExactly keeps reading until it has the count it was asked for", "[net][tcpclient]")
{
    ScriptedSocket socket { { 2, 5, 1 } };

    auto const got = SyncRun(RecvExactly(&socket, 8));
    REQUIRE(got.has_value());
    CHECK(Testing::Unwrap(got).size() == 8);
    CHECK(socket.Remaining() == 0);
}

TEST_CASE("RecvExactly reports a peer that closed before the count arrived", "[net][tcpclient]")
{
    // Zero from a read is EOF, and here it means a short frame: the peer declared
    // a length it then did not send. Retrying would wait forever.
    ScriptedSocket socket { { 3, 0 } };

    CHECK_FALSE(SyncRun(RecvExactly(&socket, 8)).has_value());
}

TEST_CASE("RecvExactly of zero bytes is not a closed peer", "[net][tcpclient]")
{
    // The distinction the compile-cache wire depends on: a miss is a reply with a
    // zero-length payload, not the absence of a reply. Draining it must not read
    // from the socket at all, because a read of zero is how EOF is spelled.
    ScriptedSocket socket { {} };

    auto const got = SyncRun(RecvExactly(&socket, 0));
    REQUIRE(got.has_value());
    CHECK(Testing::Unwrap(got).empty());
    CHECK(socket.Remaining() == 0);
}

TEST_CASE("ConnectTcp reports why a dial failed", "[net][tcpclient]")
{
    // RFC 5737 TEST-NET-1: guaranteed not to be routable, so this exercises the
    // failure path without depending on what any particular host has listening.
    // The code is not pinned -- a closed address answers with a reset on a bare
    // host and is silently dropped behind a firewall -- but it must FAIL, and it
    // must fail within the timeout rather than parking on the kernel's own retry
    // schedule, which is the whole reason the dial is non-blocking underneath.
    constexpr auto Timeout = 300ms;
    auto const started = std::chrono::steady_clock::now();
    auto const socket = SyncRun(ConnectTcp("192.0.2.1", 9, Timeout, 0ms));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(socket.has_value());
    CAPTURE(socket.error().context);
    CHECK(elapsed < Timeout * 20);
}

TEST_CASE("RecvExactly gives up on a peer that accepts and then goes silent", "[net][tcpclient]")
{
    // The property `ioTimeout` exists for, and the reason it is applied where the
    // socket is minted rather than left to the caller. A dial that succeeds says
    // only that the peer accepted; a peer that then never answers parks the
    // calling thread forever, which for the launcher turns an optional cache into
    // a build-stopping dependency.
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("no loopback listener available on this host");

    constexpr auto IoTimeout = 300ms;
    auto client = SyncRun(ConnectTcp("127.0.0.1", listener->BoundPort(), 2s, IoTimeout));
    REQUIRE(client.has_value());

    // Accept and then do nothing at all -- the connection is up, and silent.
    auto accepted = SyncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());

    auto const started = std::chrono::steady_clock::now();
    auto const got = SyncRun(RecvExactly(client->get(), 16));
    auto const elapsed = std::chrono::steady_clock::now() - started;

    CHECK_FALSE(got.has_value());
    // Generously bounded: the assertion is "it returned at all", not a latency
    // measurement, so a loaded CI runner cannot make this flaky.
    CHECK(elapsed < std::chrono::seconds { 15 });
}

TEST_CASE("A socket with a timeout armed still transfers normally", "[net][tcpclient]")
{
    // Guards the obvious over-correction: a timeout somehow applied as an
    // immediate deadline would fail every transfer and silently disable caching
    // everywhere, and the case above would still pass.
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        SKIP("no loopback listener available on this host");

    auto client = SyncRun(ConnectTcp("127.0.0.1", listener->BoundPort(), 2s, 5s));
    REQUIRE(client.has_value());
    auto accepted = SyncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());

    auto const payload = Payload(4);
    CHECK(SyncRun(SendAll(client->get(), std::span<std::byte const> { payload })));

    auto const got = SyncRun(RecvExactly(accepted->get(), payload.size()));
    REQUIRE(got.has_value());
    CHECK(Testing::Unwrap(got) == payload);
}
