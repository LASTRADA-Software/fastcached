// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Async/Task.hpp>
#include <FastCache/Cache/IStorage.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/HealthProbe.hpp>
#include <FastCache/Net/IListener.hpp>
#include <FastCache/Server/AdminHttpServer.hpp>

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>

    #include <unistd.h>

    #include <netinet/in.h>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

using namespace FastCache;

namespace
{

/// Accept exactly one connection on `listener` and return whether it succeeded.
/// A free function (not a capturing lambda) so it is a safe coroutine body.
/// @param listener The bound listener to accept on.
[[nodiscard]] Task<bool> AcceptOne(IListener* listener)
{
    auto r = co_await listener->Accept();
    co_return r.has_value();
}

/// A listener on a loopback port this run was GIVEN, plus the port it got.
///
/// **Why the port is not written down anywhere.** These cases used three fixed
/// numbers (19287, 19289, 19290), and a fixed port is a port something else on the
/// machine may hold. What that cost is the whole of
/// [#696](https://github.com/LASTRADA-Software/fastcached/issues/696): the
/// bind-failure path is a `SKIP`, so a collision did not fail anything -- it made
/// the case stop running while the suite still exited 0. Measured on this tree by
/// occupying the three ports: **6 assertions became 1, three cases reported
/// skipped, and both runs exited 0.** Before #685 it was worse still, because the
/// same path was a `SUCCEED` and a collision reported a pass.
///
/// `bind(port 0)` removes the failure mode rather than reporting it better: the
/// kernel picks a free port and binds it in one step, so there is no number for
/// anything to have taken and no window in which to lose a race.
///
/// **This is deliberately NOT the "below the ephemeral range" draw** that
/// `.agent/rules/testing.md` prescribes, and the difference is the window rather
/// than the range. That rule governs a fixture which DRAWS a number and BINDS it
/// later -- a shell script handing a port to a daemon on a command line -- where the
/// gap between the two lets an outbound connection take the number, invisibly to a
/// connect probe, and the `bind()` then fails with `EADDRINUSE`. Here there is no
/// gap to protect: the port arrives already bound, and this process holds it for as
/// long as the case runs. A drawn number would be strictly weaker.
struct BoundLoopback
{
    std::unique_ptr<BlockingListener> listener;
    std::uint16_t port { 0 };

    /// @return True when the listener is bound and the port is usable.
    [[nodiscard]] bool Ok() const noexcept
    {
        return listener != nullptr && listener->IsBound() && port != 0;
    }
};

/// Bind a loopback listener on a kernel-chosen port.
/// @return The listener and its port; `Ok()` is false when this host would not bind.
[[nodiscard]] BoundLoopback BindLoopbackPerRun()
{
    auto listener = BlockingListener::Bind("127.0.0.1", 0);
    if (listener == nullptr || !listener->IsBound())
        return {};
    auto const port = listener->BoundPort();
    if (port == 0)
        return {};
    return BoundLoopback { .listener = std::move(listener), .port = port };
}

/// What a case says when it could not get a loopback listener at all.
///
/// It stays a `SKIP` -- #685's rule is that a case which did not run must not read
/// as one that passed, and that is unchanged. What changes is what a reader may
/// conclude from it: with the port no longer written down, this can no longer mean
/// *"something else holds our number"*. The only remaining cause is a host that will
/// not give this process a loopback listener, which is an environment this suite
/// cannot test in. Do not re-add a collision reason here; there is no longer a
/// collision to have.
constexpr std::string_view NoLoopbackListener {
    "this host would not bind a loopback listener on any port; the probe cannot be exercised here"
};

/// A loopback port this process HOLDS and deliberately never listens on.
///
/// The one place in this file where "allocate a port" is the wrong instinct
/// (#696 says so explicitly): the case it serves asserts that a probe against
/// *nothing* fails, so an allocated-and-closed port would weaken it into "probably
/// nothing is there". Binding without `listen()` is the by-construction form --
/// **measured on Linux x86-64 before this was relied on**:
///
///   - `connect()` to it answers `ECONNREFUSED`, which is what the case wants to see;
///   - a second `bind()` on the same port answers `EADDRINUSE`, so nothing can take
///     it away while this object is alive.
///
/// So there is nothing listening, it cannot start listening, and no fixed number is
/// involved. `ReusePort` is deliberately not set: sharing the port is exactly what
/// would let something else answer on it.
class HeldUnlistenedPort
{
  public:
    HeldUnlistenedPort()
    {
        Detail::EnsureNetworkInitialised();
#if defined(_WIN32)
        auto const opened = ::socket(AF_INET, SOCK_STREAM, 0);
        if (opened == INVALID_SOCKET)
            return;
#else
        int const opened = ::socket(AF_INET, SOCK_STREAM, 0);
        if (opened < 0)
            return;
#endif
        auto const raw = static_cast<Detail::NativeSocket>(opened);
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // the kernel picks, and binds, in one step
        if (::bind(opened, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) != 0)
        {
            CloseRaw(raw);
            return;
        }
        sockaddr_in bound {};
#if defined(_WIN32)
        int boundLen = static_cast<int>(sizeof(bound));
#else
        socklen_t boundLen = sizeof(bound);
#endif
        if (::getsockname(opened, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0)
        {
            CloseRaw(raw);
            return;
        }
        // No listen(). That absence IS the fixture.
        _native = raw;
        _port = ntohs(bound.sin_port);
    }

    HeldUnlistenedPort(HeldUnlistenedPort const&) = delete;
    HeldUnlistenedPort(HeldUnlistenedPort&&) = delete;
    HeldUnlistenedPort& operator=(HeldUnlistenedPort const&) = delete;
    HeldUnlistenedPort& operator=(HeldUnlistenedPort&&) = delete;

    ~HeldUnlistenedPort()
    {
        if (_native != Detail::InvalidSocket)
            CloseRaw(_native);
    }

    /// @return True when the port is held and can be probed.
    [[nodiscard]] bool Ok() const noexcept
    {
        return _native != Detail::InvalidSocket && _port != 0;
    }

    /// @return The held port; 0 when `Ok()` is false.
    [[nodiscard]] std::uint16_t Port() const noexcept
    {
        return _port;
    }

  private:
    /// Close a raw handle on either platform.
    /// @param native The handle to close.
    static void CloseRaw(Detail::NativeSocket native) noexcept
    {
#if defined(_WIN32)
        std::ignore = ::closesocket(static_cast<SOCKET>(native));
#else
        std::ignore = ::close(static_cast<int>(native));
#endif
    }

    Detail::NativeSocket _native { Detail::InvalidSocket };
    std::uint16_t _port { 0 };
};

} // namespace

TEST_CASE("HttpHealthProbe succeeds against a live /healthz and fails otherwise", "[net][health]")
{
    auto bound = BindLoopbackPerRun();
    if (!bound.Ok())
        SKIP(NoLoopbackListener);
    auto const Port = bound.port;
    auto& listener = bound.listener;
    // Poll accept() so Shutdown() is observed and the server jthread joins on
    // every platform — POSIX does not unblock a parked accept() on Close(), so
    // without this the test would hang on Linux at scope exit.
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 1 });

    NullLogger logger;
    AtomicMetricsSink metrics;
    AdminHttpServer server { *listener, metrics, [] { return MetricsSnapshot {}; }, logger };

    std::jthread serverThread { [&server] { FastCache::SyncRun(server.Run()); } };

    // Give the accept loop a moment to park on Accept().
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

    CHECK(HttpHealthProbe("127.0.0.1", Port, "/healthz"));
    // A path that 404s is not "200", so the probe must report unhealthy.
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/nope"));

    server.Shutdown();
}

TEST_CASE("HttpHealthProbe fails when nothing is listening", "[net][health]")
{
    // An unbound port yields a connection refused -> unhealthy, not a hang.
    //
    // The port is HELD by this process and never listened on, so "nothing is
    // listening" is true by construction rather than by hope -- see
    // `HeldUnlistenedPort` for the two measurements that rest on. It used to be the
    // literal 19288, chosen for being one past a neighbouring case's port, which is
    // an assumption about the whole machine rather than about this test.
    HeldUnlistenedPort const nothingListening;
    if (!nothingListening.Ok())
        SKIP(NoLoopbackListener);
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", nothingListening.Port(), "/healthz"));
}

TEST_CASE("HttpHealthProbe rejects a non-200 response whose body contains \" 200 \"", "[net][health]")
{
    // A previous implementation declared the peer healthy whenever the
    // substring " 200 " appeared anywhere in the first 256 bytes — including
    // inside a 5xx error page body. The probe must parse the HTTP status line
    // strictly: "HTTP/1.x 200 ...".
    auto bound = BindLoopbackPerRun();
    if (!bound.Ok())
        SKIP(NoLoopbackListener);
    auto const Port = bound.port;
    auto& listener = bound.listener;
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 5 });

    auto const respond500 = [](IListener* l) -> Task<void> {
        auto accepted = co_await l->Accept();
        if (!accepted.has_value())
            co_return;
        std::array<char, 256> req {};
        (void) co_await (*accepted)->Read(std::span<std::byte> { reinterpret_cast<std::byte*>(req.data()), req.size() });
        constexpr std::string_view Reply { "HTTP/1.1 500 Internal Server Error\r\n"
                                           "Content-Type: text/plain\r\n"
                                           "Content-Length: 27\r\n"
                                           "\r\n"
                                           "expected 200 OK got 5xx err" };
        (void) co_await (*accepted)->Write(
            std::span<std::byte const> { reinterpret_cast<std::byte const*>(Reply.data()), Reply.size() });
        (*accepted)->Close();
    };
    std::jthread acceptor { [&listener, &respond500](std::stop_token const& stop) {
        while (!stop.stop_requested())
            FastCache::SyncRun(respond500(listener.get()));
    } };

    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));

    acceptor.request_stop();
    listener->Close();
}

TEST_CASE("HttpHealthProbe times out (not hangs) against an accept-but-silent peer", "[net][health]")
{
    // A listener that accepts the TCP connection but never sends a response: the
    // probe's recv timeout must make it return unhealthy within a few seconds
    // rather than blocking forever. We accept exactly one connection and hold it
    // open without replying.
    auto bound = BindLoopbackPerRun();
    if (!bound.Ok())
        SKIP(NoLoopbackListener);
    auto const Port = bound.port;
    auto& listener = bound.listener;
    listener->SetTimeouts(std::chrono::milliseconds { 100 }, std::chrono::seconds { 5 });

    std::jthread acceptor { [&listener](std::stop_token const& stop) {
        // Accept and then sit on the connection (never write) until asked to stop.
        auto accepted = FastCache::SyncRun(AcceptOne(listener.get()));
        static_cast<void>(accepted);
        while (!stop.stop_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    } };

    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });

    auto const start = std::chrono::steady_clock::now();
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // It must return on the bounded probe timeout (~3s), not hang indefinitely.
    CHECK(elapsed < std::chrono::seconds { 10 });

    acceptor.request_stop();
    listener->Close();
}
