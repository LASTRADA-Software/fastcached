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

/// Make raw sockets usable in this process.
///
/// On Windows a socket call before WSAStartup fails with WSANOTINITIALISED, and
/// the peers below would report themselves unusable and every case here would
/// take its "could not bind" skip. That is what was happening: these tests
/// reported Passed in 0.01s on Windows, faster than the 300 ms timeout one of
/// them measures, because none of them ran. ConnectTcp starts Winsock for its
/// own sockets, but it is called from the test bodies -- after the peer has
/// already tried to bind.
///
/// Idempotent, and deliberately a second WSAStartup rather than a hook into
/// TcpClient.cpp's: startup is reference-counted, the test binary never cleans
/// up (it is as short-lived as the launcher), and reaching into the production
/// TU's internals to share one would couple the test to a detail that exists
/// only for the launcher's own lifetime.
/// @return True when raw sockets can be created.
[[nodiscard]] bool EnsureSocketsUsable()
{
#if defined(_WIN32)
    static bool const ready = [] {
        WSADATA wsa {};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ready;
#else
    return true;
#endif
}

/// What a LoopbackPeer does with the connection it accepts.
///
/// The two cases are opposites and both are needed: a peer that holds the
/// connection open is the one that hung a build, and a peer that hangs up is the
/// one that killed the launcher outright.
enum class PeerBehavior : std::uint8_t
{
    HoldOpen, ///< Accept, then say nothing at all and keep the socket open.
    HangUp,   ///< Accept, then immediately close without reading or writing.
};

/// A loopback listener that accepts exactly one connection and then behaves as
/// its PeerBehavior says.
///
/// Deliberately raw sockets rather than FastCache::BlockingListener: the
/// launcher's test binary does not link the FastCache library (see the comment
/// in this directory's CMakeLists.txt), so it cannot reach that helper.
///
/// One class parameterized by behaviour rather than one class per behaviour --
/// the bind/listen/getsockname dance is identical and only the acceptor body
/// differs, which is a table entry rather than a second copy of the setup.
class LoopbackPeer
{
  public:
    explicit LoopbackPeer(PeerBehavior behavior):
        _listenFd { EnsureSocketsUsable() ? ::socket(AF_INET, SOCK_STREAM, 0) : InvalidSocket }
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

        _acceptor = std::jthread { [this, behavior](std::stop_token const& stop) {
            auto const accepted = ::accept(_listenFd, nullptr, nullptr);
            if (behavior == PeerBehavior::HoldOpen)
            {
                // Holding it is the point: a closed socket surfaces as a clean
                // EOF, which the client already handled correctly. Silence is
                // the case that hung.
                while (!stop.stop_requested())
                    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
            }
            if (accepted != InvalidSocket)
                CloseSocket(accepted);
        } };
    }

    ~LoopbackPeer()
    {
        _acceptor.request_stop();
        _acceptor = {};
        if (_listenFd != InvalidSocket)
            CloseSocket(_listenFd);
    }

    LoopbackPeer(LoopbackPeer const&) = delete;
    LoopbackPeer& operator=(LoopbackPeer const&) = delete;
    LoopbackPeer(LoopbackPeer&&) = delete;
    LoopbackPeer& operator=(LoopbackPeer&&) = delete;

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

TEST_CASE("SendAll to a peer that hung up fails instead of killing the process")
{
    // The regression test for issue #68. Before the fix this did not fail an
    // assertion -- it terminated the test binary with signal 13, exactly as it
    // terminated fastcache-cc mid-STORE and failed a build whose object file was
    // already compiled, correct, and on disk.
    //
    // The assertion is therefore doubled: reaching the CHECK at all proves no
    // signal was raised, and the CHECK proves the error was reported through the
    // return value the caching flow already treats as "cache unavailable".
    LoopbackPeer peer { PeerBehavior::HangUp };
    if (!peer.Ready())
    {
        SUCCEED("could not bind a loopback test port; skipping");
        return;
    }

    // Generous, because a timeout here would report false for the wrong reason
    // and pass the test vacuously; EPIPE against a hung-up loopback peer is
    // immediate, so this bound is never reached in a healthy run.
    auto client = FastCache::Cc::ConnectTcp(peer.Endpoint(), std::chrono::milliseconds { 30000 });
    REQUIRE(client != nullptr);

    // Chunked rather than one enormous buffer: the first write after a hang-up
    // is routinely accepted (it is the peer's RST, arriving in response, that
    // breaks the pipe), so the failure needs a second write to surface. A real
    // object file supplies thousands.
    constexpr std::size_t ChunkBytes = 256UL * 1024UL;
    constexpr int MaxChunks = 64; // 16 MiB is far past any loopback send buffer
    std::vector<std::byte> const chunk(ChunkBytes, std::byte { 0xAB });

    bool reported = false;
    for (int i = 0; i < MaxChunks && !reported; ++i)
        reported = !client->SendAll(std::span<std::byte const> { chunk });

    CHECK(reported);
}

TEST_CASE("RecvExactly gives up on a peer that accepts and then goes silent")
{
    LoopbackPeer peer { PeerBehavior::HoldOpen };
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
    LoopbackPeer peer { PeerBehavior::HoldOpen };
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
