// SPDX-License-Identifier: Apache-2.0
//
// What a blocking socket must do when the peer goes away.
//
// The interesting property is not that a write to a live peer works -- every
// other test in this tree leans on that -- but that a write to a peer which has
// hung up comes back as an *error* rather than as a fatal signal. That is a
// property of how the socket was set up, not of the write, so it can only be
// tested through a real connected pair.
#include <FastCache/Async/Task.hpp>
#include <FastCache/Net/BlockingConnector.hpp>
#include <FastCache/Net/BlockingSocket.hpp>

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)
    #include <csignal>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

using namespace FastCache;
using namespace std::chrono_literals;

namespace
{

/// Bind a listener on an ephemeral loopback port.
/// @return The listener, or nullptr when the platform would not bind.
[[nodiscard]] std::unique_ptr<BlockingListener> BindEphemeral()
{
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return nullptr;
    return listener;
}

/// Await one accept. A blocking listener resolves it synchronously, so `SyncRun`
/// is the right driver here -- the task is never left suspended, which is the
/// one thing `SyncRun` refuses to read from.
/// Pointers rather than references because a coroutine parameter must not be a
/// reference: the frame outlives the call expression, so a bound reference can
/// dangle. Every awaiting caller here is synchronous, but the rule is the same
/// one `RaftPeerTransport::WriteFrame` already follows.
/// @param listener The bound listener.
/// @return The accepted socket, or the accept error.
[[nodiscard]] Task<AcceptResult> AcceptOne(BlockingListener* listener)
{
    co_return co_await listener->Accept();
}

/// Await one write.
/// @param socket The connected socket.
/// @param bytes What to write.
/// @return Bytes written, or the socket error.
[[nodiscard]] Task<IoResult> WriteOnce(ISocket* socket, std::span<std::byte const> bytes)
{
    co_return co_await socket->Write(bytes);
}

} // namespace

TEST_CASE("A write to a peer that hung up fails instead of killing the process", "[net][socket]")
{
    // The regression test for the process-wide SIGPIPE disposition this file used
    // to install at start-up. Before per-socket suppression, this case did not
    // fail an assertion when the disposition was absent -- it terminated the test
    // binary with signal 13. The launcher's own copy of this test is recorded as
    // having done exactly that (issue #68), which is what makes this one worth
    // having: a regression test for a fatal signal that cannot be seen to fail is
    // worth nothing.
    //
    // So the assertion is doubled. Reaching the CHECK at all proves no signal was
    // raised, and the CHECK proves the failure came back through the return value
    // callers already handle.
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }

    BlockingConnector connector;
    auto client = connector.Connect("127.0.0.1", listener->BoundPort(), 2s);
    REQUIRE(client.has_value());

    auto accepted = SyncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());

    // The hang-up. Closing rather than shutting down the write side, because
    // what a caller meets in production is a peer process that went away.
    (*accepted)->Close();

    // Chunked rather than one enormous buffer: the first write after a hang-up is
    // routinely accepted -- it is the peer's RST, arriving in response, that
    // breaks the pipe -- so the failure needs a second write to surface. A real
    // object file supplies thousands.
    constexpr std::size_t ChunkBytes = 256UL * 1024UL;
    constexpr int MaxChunks = 64; // 16 MiB is far past any loopback send buffer
    std::vector<std::byte> const chunk(ChunkBytes, std::byte { 0xAB });

    bool reported = false;
    for (int i = 0; i < MaxChunks && !reported; ++i)
        reported = !SyncRun(WriteOnce(client->get(), std::span<std::byte const> { chunk })).has_value();

    CHECK(reported);
}

#if !defined(_WIN32)
TEST_CASE("Using a socket leaves the process SIGPIPE disposition alone", "[net][socket]")
{
    // The defect this file's per-socket suppression exists for, asserted directly
    // rather than through its consequence.
    //
    // The obvious way to keep a broken pipe from killing a server is one
    // `::signal(SIGPIPE, SIG_IGN)` when the network is first touched, and that is
    // what this file used to do. It is wrong for any process that also spawns a
    // child: an ignored disposition is INHERITED ACROSS EXEC, so the ignore stops
    // being a property of this program and becomes a property of every program it
    // launches. `fastcache-compile-node` links this library, listens on a socket
    // and then runs a compiler per job -- so it was silently handing every one of
    // those compilers a SIGPIPE disposition they never asked for, which is exactly
    // what `fastcache-cc` is documented as having to avoid for the same reason.
    //
    // Nothing observable goes wrong in the parent, which is why this needs saying
    // out loud: the daemon kept working, and only the children were affected.
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }

    BlockingConnector connector;
    auto client = connector.Connect("127.0.0.1", listener->BoundPort(), 2s);
    REQUIRE(client.has_value());

    struct sigaction current {};
    REQUIRE(::sigaction(SIGPIPE, nullptr, &current) == 0);
    CHECK(current.sa_handler == SIG_DFL);
}
#endif

TEST_CASE("A connected pair still round-trips bytes", "[net][socket]")
{
    // Guards the obvious over-correction. `MSG_NOSIGNAL` is passed on every send
    // now, and a platform where that flag were rejected would fail every write --
    // silently disabling the daemon's whole write path, which the case above
    // cannot distinguish from the success it is looking for.
    auto listener = BindEphemeral();
    if (listener == nullptr)
    {
        SUCCEED("no loopback listener available on this host");
        return;
    }

    BlockingConnector connector;
    auto client = connector.Connect("127.0.0.1", listener->BoundPort(), 2s);
    REQUIRE(client.has_value());

    auto accepted = SyncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());

    std::array<std::byte, 4> const payload { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 } };
    auto const written = SyncRun(WriteOnce(client->get(), std::span<std::byte const> { payload }));
    REQUIRE(written.has_value());
    CHECK(*written == payload.size());
}
