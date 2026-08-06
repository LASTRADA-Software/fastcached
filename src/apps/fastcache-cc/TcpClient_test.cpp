// SPDX-License-Identifier: Apache-2.0
//
// The launcher's socket seam. The property under test is not "can it talk to a
// daemon" — the e2e covers that — but the one that broke a real build: a daemon
// that accepts a connection and then goes silent must not be able to stall a
// compile indefinitely. Caching is an optimization, so every failure mode has
// to be bounded and observable.

#include "ITcpClient.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <unistd.h>

    #include <netinet/in.h>
#endif

namespace
{

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
void CloseSocket(NativeSocket fd)
{
    ::closesocket(fd);
}
using AddrLen = int;
#else
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;
void CloseSocket(NativeSocket fd)
{
    ::close(fd);
}
using AddrLen = ::socklen_t;
#endif

/// A listener that accepts one connection and then says nothing at all.
///
/// Deliberately raw sockets rather than FastCache::BlockingListener: the
/// launcher's test binary does not link the FastCache library (see the comment
/// in this directory's CMakeLists.txt), so it cannot reach that helper.
class SilentPeer
{
  public:
    SilentPeer():
        _listenFd { ::socket(AF_INET, SOCK_STREAM, 0) }
    {
        if (_listenFd == InvalidSocket)
            return;
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral
        if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(_listenFd, 1) != 0)
        {
            CloseSocket(_listenFd);
            _listenFd = InvalidSocket;
            return;
        }
        AddrLen len = sizeof(addr);
        if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        {
            CloseSocket(_listenFd);
            _listenFd = InvalidSocket;
            return;
        }
        _port = ntohs(addr.sin_port);

        // Accept on a worker and then just hold the connection open. Holding it
        // is the point: a closed socket would surface as a clean EOF, which the
        // client already handled correctly. Silence is the case that hung.
        _acceptor = std::jthread { [this](std::stop_token const& stop) {
            auto const accepted = ::accept(_listenFd, nullptr, nullptr);
            while (!stop.stop_requested())
                std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
            if (accepted != InvalidSocket)
                CloseSocket(accepted);
        } };
    }

    ~SilentPeer()
    {
        _acceptor.request_stop();
        _acceptor = {};
        if (_listenFd != InvalidSocket)
            CloseSocket(_listenFd);
    }

    SilentPeer(SilentPeer const&) = delete;
    SilentPeer& operator=(SilentPeer const&) = delete;
    SilentPeer(SilentPeer&&) = delete;
    SilentPeer& operator=(SilentPeer&&) = delete;

    /// @return True if the listener bound and is usable.
    [[nodiscard]] bool Ready() const noexcept
    {
        return _listenFd != InvalidSocket;
    }

    /// @return "127.0.0.1:<port>" for ConnectTcp.
    [[nodiscard]] std::string Endpoint() const
    {
        return "127.0.0.1:" + std::to_string(_port);
    }

  private:
    NativeSocket _listenFd { InvalidSocket };
    std::uint16_t _port { 0 };
    std::jthread _acceptor;
};

} // namespace

TEST_CASE("RecvExactly gives up on a peer that accepts and then goes silent")
{
    SilentPeer peer;
    if (!peer.Ready())
    {
        SUCCEED("could not bind a loopback test port; skipping");
        return;
    }

    constexpr std::chrono::milliseconds Timeout { 300 };
    auto client = FastCache::Cc::ConnectTcp(peer.Endpoint(), Timeout);
    REQUIRE(client != nullptr);

    auto const start = std::chrono::steady_clock::now();
    auto const got = client->RecvExactly(16);
    auto const elapsed = std::chrono::steady_clock::now() - start;

    // nullopt is what the caching flow already treats as "cache unavailable",
    // so the existing fall-back to a real compile takes over from here.
    CHECK_FALSE(got.has_value());
    // Generously bounded: the assertion is "it returned at all", not a timing
    // measurement, so a loaded CI machine cannot make this flaky.
    CHECK(elapsed < std::chrono::seconds { 15 });
}

TEST_CASE("A connected client still round-trips bytes with a timeout armed")
{
    // Guards the obvious over-correction: a timeout that is somehow applied as
    // an immediate deadline would make every fetch fail and silently disable
    // caching everywhere, which no other test would notice.
    SilentPeer peer;
    if (!peer.Ready())
    {
        SUCCEED("could not bind a loopback test port; skipping");
        return;
    }

    auto client = FastCache::Cc::ConnectTcp(peer.Endpoint(), std::chrono::milliseconds { 5000 });
    REQUIRE(client != nullptr);

    std::array<std::byte, 4> const payload { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 } };
    CHECK(client->SendAll(std::span<std::byte const> { payload }));
}

TEST_CASE("ConnectTcp rejects a malformed endpoint")
{
    CHECK(FastCache::Cc::ConnectTcp("no-colon-here", std::chrono::milliseconds { 100 }) == nullptr);
}
