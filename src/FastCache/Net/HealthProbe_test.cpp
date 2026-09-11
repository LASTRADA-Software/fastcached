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
#include <atomic>
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

/// Accept exactly one connection on `listener` and hand the SOCKET back.
/// A free function (not a capturing lambda) so it is a safe coroutine body.
///
/// **The socket rather than a `bool`, and that is the accept-but-silent case's whole
/// subject.** A coroutine answering `r.has_value()` destroys `r` on its way out, which
/// CLOSES the connection the instant it is accepted -- so the peer was accept-then-go,
/// the probe returned on EOF rather than on its own recv timeout, and the case's bound
/// was asserted against nothing. `CHECK_FALSE` and `elapsed < 10s` both hold under
/// that, which is why it stayed invisible: an accept-but-silent peer and a peer that
/// hung up were the same observation.
/// @param listener The bound listener to accept on.
/// @return The accepted socket, or nullptr when the accept did not land.
[[nodiscard]] Task<std::unique_ptr<ISocket>> AcceptOne(IListener* listener)
{
    auto r = co_await listener->Accept();
    if (!r.has_value())
        co_return nullptr;
    co_return std::move(*r);
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

/// How long a rendezvous in this file may take, and how often it re-asks.
///
/// A BOUND rather than a duration to spend. The three cases below each slept a flat
/// 100 ms to let a server thread reach its accept loop, and under load the sleep
/// elapsed first: the probe then connected to a listener nobody was accepting on and
/// the case reported the server unhealthy
/// ([#1141](https://github.com/LASTRADA-Software/fastcached/issues/1141)). Raising the
/// constant buys a slower suite and the same failure on a slower host, which is the
/// response #354 exists to refuse -- the rendezvous wants to be a CONDITION.
///
/// The bound is generous because it is only ever paid when something is actually
/// wrong; the ordinary cost is one poll interval.
constexpr std::chrono::milliseconds RendezvousBound { 10'000 };

/// How often a rendezvous re-asks its condition.
constexpr std::chrono::milliseconds RendezvousPoll { 10 };

/// What a bounded rendezvous observed, so a timeout can say what it measured.
struct Rendezvous
{
    bool ready { false };                   ///< Whether the condition held before the bound.
    std::chrono::milliseconds waited { 0 }; ///< Measured, never assumed from the poll count.
};

/// Wait until `ready()` answers true, or the bound expires.
///
/// The elapsed time is MEASURED off `steady_clock` rather than derived from the number
/// of polls: `n * RendezvousPoll` is what the loop asked for, and a sleep costs what the
/// host's timer granularity says. It is also deliberately not the wall clock -- WSL2
/// steps `CLOCK_REALTIME` both ways, so a duration read from it is not a duration
/// ([#1058](https://github.com/LASTRADA-Software/fastcached/issues/1058)).
/// The bound is checked BETWEEN attempts, so the worst case is the bound plus one
/// predicate call -- measured at **12035 ms** against a 10 s bound for the case whose
/// predicate is an `HttpHealthProbe` against a silent peer, which spends its own ~3 s
/// timeout before answering. That is why the elapsed is REPORTED rather than the bound:
/// a reader who saw only "timed out after 10000 ms" would be reading a number nobody
/// observed, which is the defect `wait_until` in `scripts/lib/e2e-common.sh` was fixed
/// for. Interrupting a predicate mid-call is not worth the machinery here -- nothing in
/// this file waits on anything whose own timeout is unbounded.
/// @param ready The condition to wait for; called until it answers true.
/// @return Whether it held, and how long this actually took.
template <typename Predicate>
[[nodiscard]] Rendezvous WaitUntilReady(Predicate ready)
{
    auto const start = std::chrono::steady_clock::now();
    auto const deadline = start + RendezvousBound;
    for (;;)
    {
        auto const held = ready();
        auto const now = std::chrono::steady_clock::now();
        auto const waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
        if (held)
            return Rendezvous { .ready = true, .waited = waited };
        if (now >= deadline)
            return Rendezvous { .ready = false, .waited = waited };
        std::this_thread::sleep_for(RendezvousPoll);
    }
}

/// Whether a TCP connect to a loopback port SUCCEEDS, sending nothing.
///
/// The discrimination a timed-out rendezvous owes its reader, and it is the whole
/// reason this is a raw connect rather than another `HttpHealthProbe`: a slow host and
/// a wedged server want different people. The listener these cases bind is listening
/// before any thread starts, so the kernel queues a connection in its backlog whether
/// or not anything is accepting -- which means a connect that SUCCEEDS says the
/// listener is alive and its accept loop never ran, and a connect that is REFUSED says
/// the listener itself is gone. Neither is inferable from the probe's `false`.
/// @param port The loopback port to reach for.
/// @return True when the connect completed.
[[nodiscard]] bool LoopbackConnectSucceeds(std::uint16_t port)
{
    Detail::EnsureNetworkInitialised();
#if defined(_WIN32)
    auto const opened = ::socket(AF_INET, SOCK_STREAM, 0);
    if (opened == INVALID_SOCKET)
        return false;
#else
    int const opened = ::socket(AF_INET, SOCK_STREAM, 0);
    if (opened < 0)
        return false;
#endif
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    auto const connected = ::connect(opened, reinterpret_cast<sockaddr const*>(&addr), sizeof(addr)) == 0;
#if defined(_WIN32)
    std::ignore = ::closesocket(opened);
#else
    std::ignore = ::close(opened);
#endif
    return connected;
}

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
    SteadyClock clock;
    AdminHttpServer server { *listener, metrics, [] { return MetricsSnapshot {}; }, logger, clock };

    std::atomic<bool> threadRan { false };
    std::jthread serverThread { [&server, &threadRan] {
        threadRan.store(true, std::memory_order_release);
        FastCache::SyncRun(server.Run());
    } };

    // The rendezvous IS the positive assertion, and that is deliberate: waiting until
    // the server answers 200 and waiting until it is up are the same question, so
    // spelling them separately would only add a second reading of one fact. What a
    // duration could not do is fail INFORMATIVELY, which is the whole of #1141.
    auto const live = WaitUntilReady([Port] { return HttpHealthProbe("127.0.0.1", Port, "/healthz"); });
    if (!live.ready)
    {
        auto const reachable = LoopbackConnectSucceeds(Port);
        // Stopped BEFORE the `FAIL`, and after the diagnosis has been taken: `FAIL`
        // aborts the case by THROWING, so the `server.Shutdown()` at the end of this
        // body is unwound past, and `~jthread` then joins an accept loop nothing has
        // asked to stop -- `AdminHttpServer::Run` leaves only on the shutdown flag, and
        // the lambda takes no stop token for `request_stop` to reach. The case would
        // HANG on the one path this diagnosis exists to serve.
        server.RequestStop();
        serverThread.join();
        server.Shutdown();
        FAIL("the admin server never answered /healthz, measured over "
             << live.waited.count() << " ms. A TCP connect to its port "
             << (reachable ? "SUCCEEDS, so the listener is bound and nothing accepted -- the server thread is "
                             "wedged or was never scheduled"
                           : "is REFUSED, so the listener itself is gone -- not a scheduling problem")
             << ", and the server thread " << (threadRan.load(std::memory_order_acquire) ? "did" : "did NOT")
             << " begin running.");
    }

    // A path that 404s is not "200", so the probe must report unhealthy. This is the
    // half that makes the case a discrimination rather than a liveness check: a probe
    // that answered true for everything would pass the line above.
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/nope"));

    // Ask, JOIN, then close — and this is the site the ThreadSanitizer report in
    // [#1207](https://github.com/LASTRADA-Software/fastcached/issues/1207) actually
    // names, which is worth saying because it is not the one it looks like.
    // `Shutdown()` closes the listener, and `~jthread` joins only at scope exit, so a
    // bare `Shutdown()` writes `_native` while this case's own server thread is
    // reading it inside `Accept()`.
    //
    // `RequestStop()` exists for exactly this and says so in its own Doxygen — *"call
    // this, join the accept thread, and only then call `Shutdown()`"* — so what failed
    // here was not a missing contract but an unenforced one. The join is bounded by
    // the 100 ms accept poll armed above; closing could never have shortened it,
    // because POSIX does not unblock a parked `accept()` from another thread.
    server.RequestStop();
    serverThread.join();
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

    auto const respond500 = [](IListener* l) -> Task<bool> {
        auto accepted = co_await l->Accept();
        if (!accepted.has_value())
            co_return false;
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
        co_return true;
    };
    // `answered` is what stops this case passing vacuously. The assertion below is a
    // `CHECK_FALSE`, and an unanswered probe is `false` too -- so with no acceptor at
    // all, or an acceptor that never got scheduled, the case reported the rule holding
    // over a peer that had said nothing (#1141, the same file's #354 shape).
    std::atomic<bool> reachedAccept { false };
    std::atomic<bool> answered { false };
    std::jthread acceptor { [&listener, &respond500, &reachedAccept, &answered](std::stop_token const& stop) {
        while (!stop.stop_requested())
        {
            reachedAccept.store(true, std::memory_order_release);
            if (FastCache::SyncRun(respond500(listener.get())))
                answered.store(true, std::memory_order_release);
        }
    } };

    auto const parked = WaitUntilReady([&reachedAccept] { return reachedAccept.load(std::memory_order_acquire); });
    INFO("waited " << parked.waited.count() << " ms for the responder thread to reach Accept()");
    REQUIRE(parked.ready);

    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));

    // It answered, so the `false` above is a REFUSAL of a 500 rather than the silence
    // of a peer that was never there. What this still cannot separate is a 500 the
    // probe read from a 500 written after the probe gave up; the responder writes
    // immediately on accept, so the two are not distinguishable from outside without a
    // status the probe does not return.
    auto const served = WaitUntilReady([&answered] { return answered.load(std::memory_order_acquire); });
    INFO("waited " << served.waited.count() << " ms for the responder to report writing its 500");
    CHECK(served.ready);

    // Ask, JOIN, then close -- the order is the fix
    // ([#1207](https://github.com/LASTRADA-Software/fastcached/issues/1207)). Closing
    // wrote `_native` while this acceptor was reading it inside `Accept()`, which
    // ThreadSanitizer reports at `BlockingListener::Close`. The join costs at most one
    // 100 ms accept poll -- the one `SetTimeouts` armed above -- because a close cannot
    // shorten the wait: POSIX does not unblock a parked `accept()` when another thread
    // closes the socket. `AdminEndpoint::~AdminEndpoint` is the same three lines with
    // the argument written out, from #260.
    acceptor.request_stop();
    acceptor.join();
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

    // Both flags earn their place. `reachedAccept` replaces a 100 ms sleep with the
    // condition it was approximating, and `accepted` is what stops the case passing
    // vacuously: the assertion is a `CHECK_FALSE`, so with NO acceptor at all the probe
    // still answers false and the case still passed -- an accept-but-silent peer and no
    // peer were the same observation (#1141).
    std::atomic<bool> reachedAccept { false };
    std::atomic<bool> accepted { false };
    std::jthread acceptor { [&listener, &reachedAccept, &accepted](std::stop_token const& stop) {
        // Accept and then SIT on the connection (never write, never close) until asked
        // to stop. Holding `taken` is what makes the peer silent rather than gone --
        // see `AcceptOne` for what dropping it costs.
        //
        // The accept RETRIES rather than being attempted once. `SetTimeouts` arms a
        // 100 ms accept poll, so a single attempt hands the probe a 100 ms window to
        // connect in and reports "never accepted" on any host that misses it -- a red
        // for a slow machine, which is the response #354 exists to refuse. Each failed
        // pass costs one poll interval, so the loop does not spin.
        std::unique_ptr<ISocket> taken;
        reachedAccept.store(true, std::memory_order_release);
        while (!stop.stop_requested())
        {
            if (taken == nullptr)
            {
                taken = FastCache::SyncRun(AcceptOne(listener.get()));
                if (taken != nullptr)
                    accepted.store(true, std::memory_order_release);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
        }
    } };

    auto const parked = WaitUntilReady([&reachedAccept] { return reachedAccept.load(std::memory_order_acquire); });
    INFO("waited " << parked.waited.count() << " ms for the silent acceptor to reach Accept()");
    REQUIRE(parked.ready);

    auto const start = std::chrono::steady_clock::now();
    CHECK_FALSE(HttpHealthProbe("127.0.0.1", Port, "/healthz"));
    auto const elapsed = std::chrono::steady_clock::now() - start;
    // It must return on the bounded probe timeout (~3s), not hang indefinitely.
    CHECK(elapsed < std::chrono::seconds { 10 });
    // And the FLOOR is what discriminates; the ceiling above never could. A peer that
    // accepted and hung up answers `false` in well under a millisecond and satisfies
    // the ceiling -- measured on this tree at **0.5 ms**, against **3.0 s** once the
    // acceptor holds the connection open. A `steady_clock` interval only ever GROWS on
    // a slow host, so a floor cannot be flaked by one; it sits well under the probe's
    // own ~3 s so that constant is not pinned from out here.
    CHECK(elapsed >= std::chrono::seconds { 1 });

    // And it timed out against a peer that had ACCEPTED it, which is the case's subject.
    auto const took = WaitUntilReady([&accepted] { return accepted.load(std::memory_order_acquire); });
    INFO("waited " << took.waited.count() << " ms for the acceptor to report taking the connection");
    CHECK(took.ready);

    // Ask, JOIN, then close -- #1207, the sibling of the site above. This one was never
    // observed failing, and that is the reason to fix it in the same change rather than
    // only the site the report named: it is byte-for-byte the same arrangement, and a
    // first failure masks its identical siblings until the day the fix relocates it.
    // The join is bounded by the acceptor's own 50 ms sleep, not by the accept poll,
    // because by here it is holding `taken` rather than parked in `Accept()`.
    acceptor.request_stop();
    acceptor.join();
    listener->Close();
}
