// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/EpollConnector.hpp>

#if defined(__linux__)

    #include <FastCache/Async/DeadlineTimer.hpp>
    #include <FastCache/Async/EpollReactor.hpp>
    #include <FastCache/Async/ResumeOn.hpp>
    #include <FastCache/Async/Task.hpp>
    #include <FastCache/Core/Clock.hpp>
    #include <FastCache/Net/EpollSocket.hpp>
    #include <FastCache/Net/TcpClient.hpp>

    #include <catch2/catch_test_macros.hpp>

    #include <sys/socket.h>

    #include <array>
    #include <chrono>
    #include <cstddef>
    #include <cstdint>
    #include <memory>
    #include <optional>
    #include <span>
    #include <string>
    #include <vector>

    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <tests/Unwrap.hpp>

using namespace std::chrono_literals;

namespace
{

/// Bind a loopback listener on an ephemeral port, or nullptr when this host will
/// not allow it. Ports are never hard-coded here: a fixed one collides with
/// whatever else the machine is doing and the failure reads as this feature
/// being broken.
[[nodiscard]] std::unique_ptr<FastCache::EpollListener> BindEphemeral(FastCache::EpollReactor& reactor)
{
    auto listener = FastCache::EpollListener::Bind(reactor, "127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return nullptr;
    return listener;
}

/// Read `want` bytes and record them.
///
/// Its own task so it can PARK on an empty socket while the caller goes on to
/// send. That park is the whole point: `EpollSocket::Read` tries `recv` first and
/// only suspends on EAGAIN, so a read that finds data or EOF waiting never
/// touches epoll at all -- and a socket the reactor was never told about would
/// answer those perfectly well. Only a read with nothing to return proves the
/// registration exists.
FastCache::DetachedTask ReadInto(FastCache::ISocket* client, std::size_t want, std::vector<std::byte>* seen, bool* done)
{
    if (auto received = co_await FastCache::RecvExactly(client, want); received.has_value())
        *seen = *received;
    *done = true;
    co_return;
}

/// Dial, accept, and exchange one payload in the direction that must park.
///
/// A free function taking raw pointers, not a capturing lambda: a coroutine
/// closure outlives the expression that created it, so captures are a
/// use-after-free waiting to happen -- the shape `RaftPeerServer::ServePeer`
/// already uses for the same reason.
FastCache::DetachedTask DriveExchange(FastCache::EpollReactor* loop,
                                      FastCache::EpollConnector* dialer,
                                      FastCache::EpollListener* server,
                                      std::uint16_t port,
                                      std::optional<FastCache::SocketResult>* out,
                                      std::vector<std::byte>* seen)
{
    // Armed before the dial, so the accept is already pending when the SYN
    // arrives rather than racing it.
    auto pending = server->Accept();

    *out = co_await dialer->Connect("127.0.0.1", port, FastCache::DialOptions { .connectTimeout = 5s });
    auto& dialResult = out->value();
    if (dialResult.has_value() && dialResult.value() != nullptr)
    {
        constexpr std::array<std::byte, 4> payload {
            std::byte { 'p' }, std::byte { 'i' }, std::byte { 'n' }, std::byte { 'g' }
        };

        auto accepted = co_await pending;
        if (accepted.has_value())
        {
            auto* const client = dialResult.value().get();
            auto* const peer = accepted.value().get();

            auto readDone = false;
            ReadInto(client, payload.size(), seen, &readDone);

            // The reader is parked on an empty socket by now, so this send is
            // what has to wake it -- through the reactor.
            (void) co_await FastCache::SendAll(peer, payload);

            while (!readDone)
                co_await FastCache::ResumeOn { *loop };

            peer->Close();
        }
        dialResult.value()->Close();
    }
    server->Close();
    loop->Stop();
    co_return;
}

} // namespace

TEST_CASE("A reactor dial connects and then actually transfers bytes", "[net][epollconnector]")
{
    // The single most valuable case here, and the reason it does not stop at
    // "connected": the dial attaches its own handler to the fd, and EpollSocket's
    // constructor attaches the SAME fd again -- which epoll refuses with EEXIST,
    // and which EpollSocket ignores. The result would be a socket the reactor
    // never watches, whose every read parks forever, with nothing logged. A dial
    // that only asserted success would pass against exactly that.
    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };

    auto listener = BindEphemeral(reactor);
    if (listener == nullptr)
        SKIP("no loopback listener available on this host");

    FastCache::InlineAddressResolver resolver;
    FastCache::EpollConnector connector { reactor, resolver, clock };

    std::optional<FastCache::SocketResult> dialed;
    std::vector<std::byte> echoed;

    DriveExchange(&reactor, &connector, listener.get(), listener->BoundPort(), &dialed, &echoed);

    // Bounded, not merely expected to finish. The defect this case exists for --
    // a socket the reactor never watches, because the dial's own handler was
    // still attached to the fd -- presents as a read that parks forever, so
    // without a watchdog it would report as a suite timeout naming nothing rather
    // than as this assertion. `DeadlineTimer` is the same mechanism the dial uses.
    FastCache::DeadlineTimer const watchdog {
        reactor, clock.Now() + 15s, [](void* state) { static_cast<FastCache::EpollReactor*>(state)->Stop(); }, &reactor
    };

    reactor.Run();

    REQUIRE(dialed.has_value());
    auto const& outcome = FastCache::Testing::Unwrap(dialed);
    INFO("dial outcome: " << (outcome.has_value() ? std::string { "connected" } : outcome.error().ToString()));
    REQUIRE(outcome.has_value());
    CHECK(outcome.value() != nullptr);
    // The bytes are the point: they prove the socket is really on the reactor.
    CHECK(echoed.size() == 4);
}

TEST_CASE("A dial that must wait for readiness still ends on the reactor", "[net][epollconnector]")
{
    // The case above never reaches the readiness path at all: a loopback connect
    // to a listening socket completes INLINE, so `::connect` returns 0 and the
    // attach/park/settle block is skipped entirely. Verified by removing the
    // handler detach and watching that case still pass.
    //
    // So this one forces `EINPROGRESS` the only way that is deterministic on
    // loopback: fill the listener's accept queue, so the kernel stops completing
    // handshakes, then drain it so the parked dial can finish. That is what
    // exercises Attach, UpdateInterest, the park and SettleDial at all -- without
    // it the whole readiness half of the dial is dead code as far as the suite is
    // concerned.
    FastCache::SteadyClock clock;
    FastCache::EpollReactor reactor { clock };

    auto listener = BindEphemeral(reactor);
    if (listener == nullptr)
        SKIP("no loopback listener available on this host");
    auto const port = listener->BoundPort();

    // Backlog fillers, deliberately never accepted. Raw sockets, because the
    // point is to occupy the kernel's queue rather than to talk.
    std::vector<FastCache::Detail::OwnedNativeSocket> fillers;
    for (auto attempt = 0; attempt < 64; ++attempt)
    {
        FastCache::Detail::OwnedNativeSocket filler { static_cast<FastCache::Detail::NativeSocket>(
            ::socket(AF_INET, SOCK_STREAM, 0)) };
        if (!filler.Valid())
            break;
        sockaddr_in target {};
        target.sin_family = AF_INET;
        target.sin_port = htons(port);
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(static_cast<int>(filler.Get()), reinterpret_cast<sockaddr const*>(&target), sizeof(target)) != 0)
            break;
        fillers.push_back(std::move(filler));
    }

    FastCache::InlineAddressResolver resolver;
    FastCache::EpollConnector connector { reactor, resolver, clock };

    std::optional<FastCache::SocketResult> dialed;
    auto driver = [](FastCache::EpollReactor* loop,
                     FastCache::EpollConnector* dialer,
                     FastCache::EpollListener* server,
                     std::uint16_t target,
                     std::optional<FastCache::SocketResult>* out) -> FastCache::DetachedTask {
        // Draining runs concurrently with the dial, so whichever way the kernel
        // decides to treat the overflowing SYN, the dial eventually completes.
        auto drain = [](FastCache::EpollListener* accepting, int count) -> FastCache::DetachedTask {
            for (auto taken = 0; taken < count; ++taken)
            {
                auto accepted = co_await accepting->Accept();
                if (!accepted.has_value())
                    break;
                accepted.value()->Close();
            }
            co_return;
        };
        drain(server, 96);

        *out = co_await dialer->Connect("127.0.0.1", target, FastCache::DialOptions { .connectTimeout = 20s });
        if (out->has_value())
        {
            auto& result = out->value();
            if (result.has_value() && result.value() != nullptr)
                result.value()->Close();
        }
        server->Close();
        loop->Stop();
        co_return;
    };

    driver(&reactor, &connector, listener.get(), port, &dialed);

    FastCache::DeadlineTimer const watchdog {
        reactor, clock.Now() + 30s, [](void* state) { static_cast<FastCache::EpollReactor*>(state)->Stop(); }, &reactor
    };
    reactor.Run();

    REQUIRE(dialed.has_value());
    auto const& outcome = FastCache::Testing::Unwrap(dialed);
    INFO("dial outcome: " << (outcome.has_value() ? std::string { "connected" } : outcome.error().ToString()));
    CHECK(outcome.has_value());
}

#endif // __linux__
