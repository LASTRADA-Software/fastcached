// SPDX-License-Identifier: Apache-2.0
#if defined(_WIN32)

    #include <FastCache/Async/IocpReactor.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Bytes.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/BlockingSocket.hpp>
    #include <FastCache/Net/IocpSocket.hpp>

    #include <winsock2.h>

    #include <catch2/catch_test_macros.hpp>

    #include <array>
    #include <atomic>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <span>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <vector>

    #include <ws2tcpip.h>

namespace
{

/// Connect a fresh client socket to localhost:port using blocking
/// Winsock so the test can drive it from its own thread.
std::uintptr_t ConnectClient(std::uint16_t port)
{
    auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCKET);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    auto const rc = ::connect(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr));
    REQUIRE(rc == 0);
    return static_cast<std::uintptr_t>(sock);
}

/// Find a free ephemeral port by binding a probe socket, reading the
/// assigned port, and closing.
std::uint16_t FindFreePort()
{
    auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != INVALID_SOCKET);
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    REQUIRE(::bind(sock, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) == 0);
    int len = sizeof(addr);
    REQUIRE(::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    auto const port = ntohs(addr.sin_port);
    ::closesocket(sock);
    return port;
}

FastCache::DetachedTask Echo(FastCache::IocpReactor& reactor, FastCache::IocpListener& listener, std::string& peerOut)
{
    auto accept = co_await listener.Accept();
    if (!accept.has_value())
    {
        reactor.Stop();
        co_return;
    }
    auto socket = std::move(*accept);
    // AcceptEx wrote the peer sockaddr into its output buffer; the IOCP listener
    // parses it out via GetAcceptExSockaddrs so the socket can report it.
    peerOut = socket->PeerAddress();

    // Read up to 64 bytes, then echo them back.
    std::array<std::byte, 64> buf {};
    auto const r = co_await socket->Read(std::span<std::byte> { buf.data(), buf.size() });
    if (r.has_value() && *r > 0)
        (void) co_await socket->Write(std::span<std::byte const> { buf.data(), *r });
    socket->Close();
    reactor.Stop();
    co_return;
}

} // namespace

TEST_CASE("IocpReactor + IocpListener + IocpSocket round-trip", "[reactor][iocp][socket]")
{
    FastCache::Detail::EnsureNetworkInitialised();

    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    auto const port = FindFreePort();
    auto listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", port);
    REQUIRE(listener);
    REQUIRE(listener->IsBound());

    std::string peer;
    Echo(reactor, *listener, peer);

    // Client lives on a separate thread so the reactor thread (this one)
    // can drive the accept + read + write.
    std::string response;
    std::jthread client { [port, &response] {
        auto const sock = ConnectClient(port);
        std::string_view const msg = "ping!";
        (void) ::send(static_cast<SOCKET>(sock), msg.data(), static_cast<int>(msg.size()), 0);
        std::array<char, 64> buf {};
        auto const got = ::recv(static_cast<SOCKET>(sock), buf.data(), static_cast<int>(buf.size()), 0);
        if (got > 0)
            response.assign(buf.data(), buf.data() + got);
        ::closesocket(static_cast<SOCKET>(sock));
    } };

    reactor.Run();
    client.join();
    REQUIRE(response == "ping!");
    // The client connected from the IPv4 loopback, so AcceptEx's peer address
    // resolves to 127.0.0.1 (port omitted by FormatPeerAddress).
    REQUIRE(peer == "127.0.0.1");
}

// The listener releases its listening socket when it is destroyed, whether or
// not `Close()` was called first.
//
// Worth a test because the code says the opposite at a glance: `~IocpListener`
// is `= default`, and `Close()` is what closes the socket -- which is exactly
// the shape that WAS a defect in `EpollListener` and `KqueueListener`, fixed in
// #464. Here it is not one, because `Impl::~Impl()` closes both `listenSock`
// and a half-built `current.acceptSock`, so destruction already does strictly
// more than `Close()`. Reading `= default` and stopping there is how this got
// filed as a leak (#465) that it never was.
//
// Asserted through the PORT rather than the handle, which is the Windows
// spelling of what #464's epoll test does with `fcntl` on the raw descriptor:
// `Detail::BindAndListen` claims the address with `SO_EXCLUSIVEADDRUSE`, so a
// listening socket that outlived its owner would refuse the second bind with
// `WSAEADDRINUSE`. Verified to do exactly that against a deliberately leaking
// `~Impl`, so this passing is a reading and not a vacuum.
//
// It does NOT cover the separate, real hazard that an in-flight `AcceptEx`
// completion is delivered after the owner is gone; `Close()` does not prevent
// that either, so it is not a destructor question at all.
TEST_CASE("An IocpListener destroyed without Close releases its listening socket", "[net][iocp][listener]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };

    std::uint16_t port = 0;
    {
        auto const listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", 0);
        REQUIRE(listener);
        INFO("BindError: " << listener->BindError());
        REQUIRE(listener->IsBound());
        port = listener->BoundPort();
        REQUIRE(port != 0);
        // Deliberately no Close(): that omission is the whole question.
    }

    auto const rebound = FastCache::IocpListener::Bind(reactor, "127.0.0.1", port);
    REQUIRE(rebound);
    INFO("rebind error: " << rebound->BindError());
    REQUIRE(rebound->IsBound());
    rebound->Close();
}

namespace
{

/// Drain the port for a bounded moment, so a completion queued by a teardown has
/// somewhere to be delivered.
///
/// Bounded and said out loud: every one of the cases below is a use-after-free if
/// the fix regresses, and a use-after-free that is never dequeued is one that
/// reports nothing. The reactor must actually run, and it must stop by itself.
/// @param reactor The reactor to pump.
void DrainCompletions(FastCache::IocpReactor& reactor)
{
    std::jthread const stopper { [&reactor] {
        std::this_thread::sleep_for(std::chrono::milliseconds { 250 });
        reactor.Stop();
    } };
    reactor.Run();
}

/// A connected loopback pair made with plain Winsock, so nothing else is under
/// test. The accepted end is returned for wrapping; the client end is closed by
/// the caller.
struct RawPair
{
    SOCKET client { INVALID_SOCKET };
    SOCKET served { INVALID_SOCKET };

    RawPair()
    {
        auto const acceptor = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        REQUIRE(acceptor != INVALID_SOCKET);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        REQUIRE(::bind(acceptor, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) == 0);
        REQUIRE(::listen(acceptor, 1) == 0);
        int len = sizeof(addr);
        REQUIRE(::getsockname(acceptor, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        client = static_cast<SOCKET>(ConnectClient(ntohs(addr.sin_port)));
        served = ::accept(acceptor, nullptr, nullptr);
        REQUIRE(served != INVALID_SOCKET);
        ::closesocket(acceptor);
    }

    ~RawPair()
    {
        if (client != INVALID_SOCKET)
            ::closesocket(client);
    }

    RawPair(RawPair const&) = delete;
    RawPair& operator=(RawPair const&) = delete;
    RawPair(RawPair&&) = delete;
    RawPair& operator=(RawPair&&) = delete;
};

} // namespace

// The three cases below were the probes that established what #465 actually was.
// Each one segfaulted against master before the fix; they are the acceptance
// tests for it, and they are worth keeping because the failure they guard is a
// use-after-free that no counter and no ordinary test can see.
//
// All three destroy the object BEFORE the reactor runs, so the destructor's
// same-thread assertion is satisfied by `Running()` being false -- which is also
// the shape a test or a shutdown path legitimately takes.

TEST_CASE("An IocpListener destroyed with an AcceptEx in flight survives the completion", "[net][iocp][listener]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    {
        auto const listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", 0);
        REQUIRE(listener);
        REQUIRE(listener->IsBound());
        // Accept() submits AcceptEx synchronously, so discarding the awaitable
        // still leaves the kernel holding &AcceptOp::completion.
        auto const pending = listener->Accept();
        REQUIRE_FALSE(pending.await_ready());
    }
    DrainCompletions(reactor);
    SUCCEED("the aborted AcceptEx completion was dispatched onto a live block");
}

TEST_CASE("Calling Close first is not what makes an in-flight AcceptEx safe", "[net][iocp][listener]")
{
    // The distinction this case exists to pin: `Close()` closes the socket, which
    // is what ABORTS the operation -- it does not retract the completion, and it
    // does not keep the block alive. #465 was originally filed prescribing a
    // destructor that "does what Close() does", which would have changed nothing.
    // If that reasoning ever comes back, this case and the one above fail together
    // rather than this one passing on its own.
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    {
        auto const listener = FastCache::IocpListener::Bind(reactor, "127.0.0.1", 0);
        REQUIRE(listener);
        REQUIRE(listener->IsBound());
        auto const pending = listener->Accept();
        REQUIRE_FALSE(pending.await_ready());
        listener->Close();
    }
    DrainCompletions(reactor);
    SUCCEED("survived with Close() called first, as it does without");
}

TEST_CASE("An IocpSocket destroyed with a WSARecv in flight survives the completion", "[net][iocp][socket]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    {
        FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
        std::array<std::byte, 64> buf {};
        auto const pending = sock.Read(std::span<std::byte> { buf.data(), buf.size() });
        // Nothing has been sent, so the receive is genuinely outstanding. Without
        // this the case could pass having tested a synchronous completion.
        REQUIRE_FALSE(pending.await_ready());
    }
    DrainCompletions(reactor);
    SUCCEED("the aborted WSARecv completion was dispatched onto a live block");
}

// The second defect #465 turned up, and a different failure entirely: not a
// crash but silent corruption on the wire.
//
// WSASend references the WSABUF array and the payload bytes rather than copying
// them, so they must live until the completion is dequeued. They used to be
// released "at socket teardown", which is earlier than that.
//
// Asserted through OWNERSHIP rather than by racing the wire: the payload is held
// by a shared_ptr this case watches, so "was it still alive when the socket went
// away" is a question with an exact answer at an exact moment. Reading the peer's
// bytes instead would be timing-dependent and would pass under the bug whenever
// the send happened to finish first.
TEST_CASE("An IocpSocket destroyed mid-write holds the payload until the kernel is done", "[net][iocp][socket]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    // Small send buffer and a peer that never reads, so the write cannot drain
    // and the operation is still outstanding when the socket is destroyed.
    int const smallSnd = 4096;
    ::setsockopt(pair.served, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char const*>(&smallSnd), sizeof(smallSnd));
    int const smallRcv = 4096;
    ::setsockopt(pair.client, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char const*>(&smallRcv), sizeof(smallRcv));

    auto payload = std::make_shared<std::vector<std::byte>>(8U * 1024U * 1024U, std::byte { 0xAB });
    std::weak_ptr<void const> const observer { payload };
    std::array<std::span<std::byte const>, 1> const segments { std::span<std::byte const> { *payload } };

    {
        FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
        auto const pending = sock.WriteVectored(segments, payload);
        // The write must be genuinely asynchronous, or this case proves nothing.
        REQUIRE_FALSE(pending.await_ready());
        payload.reset();
        REQUIRE_FALSE(observer.expired()); // the op holds it now
    }

    // The moment that matters. Before the fix the payload was freed here, while
    // WSASend was still reading it.
    REQUIRE_FALSE(observer.expired());

    DrainCompletions(reactor);

    // And it is released once the completion has run -- so the fix keeps it alive
    // exactly as long as it must, rather than leaking it.
    REQUIRE(observer.expired());
}

#endif // _WIN32
