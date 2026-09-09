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
    #include <optional>
    #include <span>
    #include <string>
    #include <string_view>
    #include <thread>
    #include <vector>

    #include <ws2tcpip.h>

    #include <tests/Unwrap.hpp>

namespace
{

/// Connect a fresh client socket to localhost:port using blocking
/// Winsock so the test can drive it from its own thread.
std::uintptr_t ConnectClient(std::uint16_t port)
{
    auto sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(sock != FastCache::InvalidSocketValue);
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
    REQUIRE(sock != FastCache::InvalidSocketValue);
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

/// How a parked `Read` ended, or that it never did.
///
/// One struct rather than three out-parameters, because three same-shaped pointers
/// in a row is a call nobody can read and clang-tidy's
/// `bugprone-easily-swappable-parameters` is right about it.
struct ReadOutcome
{
    /// Set the instant the await returns -- the whole observation for #884.
    bool resumed { false };

    /// How it ended. **Disengaged until the await returns**, and disengaged on the
    /// success path too, so an assertion about it cannot pass against a coroutine
    /// that never resumed -- which is exactly what these cases are about.
    /// Engaged-with-a-value and never-engaged are two states and a bare enumerator
    /// collapses them.
    std::optional<FastCache::NetErrorCode> code {};

    /// The raw OS code behind `code`, so a failure says WHICH system error rather than
    /// only that the taxonomy fell through to `SystemError`.
    int systemCode { 0 };

    /// Bytes the read returned, meaningful only where `code` is disengaged and
    /// `resumed` is set. It is here because "resumed successfully" and "resumed with
    /// the bytes its OWN operation was carrying" are different claims, and only the
    /// second one distinguishes a read answered by another operation's completion.
    std::size_t bytes { 0 };
};

// Pointers, not references: a coroutine parameter that is a reference dangles the
// moment the caller's object goes, and clang-tidy refuses it
// (cppcoreguidelines-avoid-reference-coroutine-parameters). The rest of this tree
// spells the same seam `ISocket*` / `IClock*` for the same reason.
/// Park a coroutine on a real `Read` and record how it ends.
///
/// @param sock The socket to read from.
/// @param buf Destination, owned by the caller so it outlives the coroutine.
/// @param out Where the ending is recorded, owned by the caller for the same reason.
FastCache::DetachedTask ParkOnRead(FastCache::IocpSocket* sock, std::array<std::byte, 64>* buf, ReadOutcome* out)
{
    auto const r = co_await sock->Read(std::span<std::byte> { buf->data(), buf->size() });
    out->resumed = true;
    if (r.has_value())
        out->bytes = r.value();
    else
    {
        out->code = r.error().code;
        out->systemCode = r.error().systemCode;
    }
    co_return;
}

FastCache::DetachedTask Echo(FastCache::IocpReactor* reactor, FastCache::IocpListener* listener, std::string* peerOut)
{
    auto accept = co_await listener->Accept();
    if (!accept.has_value())
    {
        reactor->Stop();
        co_return;
    }
    auto socket = std::move(*accept);
    // AcceptEx wrote the peer sockaddr into its output buffer; the IOCP listener
    // parses it out via GetAcceptExSockaddrs so the socket can report it.
    *peerOut = socket->PeerAddress();

    // Read up to 64 bytes, then echo them back.
    std::array<std::byte, 64> buf {};
    auto const r = co_await socket->Read(std::span<std::byte> { buf.data(), buf.size() });
    if (r.has_value() && *r > 0)
        (void) co_await socket->Write(std::span<std::byte const> { buf.data(), *r });
    socket->Close();
    reactor->Stop();
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
    Echo(&reactor, listener.get(), &peer);

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
        REQUIRE(acceptor != FastCache::InvalidSocketValue);
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
        REQUIRE(served != FastCache::InvalidSocketValue);
        ::closesocket(acceptor);
    }

    ~RawPair()
    {
        if (client != FastCache::InvalidSocketValue)
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

// #884: `CancelRead()` must retire a parked read BEFORE it returns, on every
// transport that can park one.
//
// **The observation is the contract itself, and it needs no seam.** Epoll and kqueue
// detach the parked awaitable and `Complete` it with `Cancelled` inline, so the read
// slot is free the instant the call returns. IOCP cannot today: the kernel owns the
// read op's single `OVERLAPPED`, so `CancelRead` can only issue `CancelIoEx` and leave
// the awaitable to be completed when the aborted completion is dequeued -- on a LATER
// reactor turn. `IoAwaitable::await_ready` already publishes whether `Complete` has
// run, so the difference is visible through the public interface rather than through a
// test-only accessor. A test that had to add one to see this would have changed the
// subject slightly.
//
// **Same reactor turn is a property of the harness, not of timing.** The reactor is
// single-threaded and nothing here drains it between the park and the assertion, so
// there is no sleep, no wire race, and no run in which this accidentally passes. That
// is the discipline the mid-write case above states: a question with an exact answer
// at an exact moment.
//
// **It deliberately does NOT arm a second read.** The reuse #884 describes is reachable
// only by double-arming, and `Detail::ClaimReadSlot`'s assertion -- the tripwire for a
// DIFFERENT defect (#663) -- stands in front of it in any build where `assert` is live.
// A reproduction that double-armed would abort in the right file, within two lines of
// the right site, for the wrong reason, and would read exactly like a successful
// reproduction. So this one observes the CAUSE, that the slot was never freed, which
// both distinguishes it from #663 and makes that confusion unreachable rather than
// merely navigated.
TEST_CASE("CancelRead retires a parked read before it returns", "[net][iocp][socket][cancelread]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
    std::array<std::byte, 64> buf {};
    ReadOutcome read;

    ParkOnRead(&sock, &buf, &read);

    // Genuinely parked. Without this the case could pass having observed a read that
    // completed synchronously and was never cancelled at all.
    REQUIRE_FALSE(read.resumed);

    sock.CancelRead();

    // The assertion. No drain stands between this and the line above.
    CHECK(read.resumed);
    // Not merely THAT it resumed: with WHICH answer. `Cancelled` is what epoll and
    // kqueue complete a retired read with, and a retirement that resumed the waiter as
    // anything else -- a spurious EOF, a peer error -- would be a different defect
    // wearing this one's passing test.
    // `CHECK` and a guard rather than `REQUIRE`, so the case reaches the control after
    // the drain below. A `REQUIRE` here aborts on the red -- which is precisely the run
    // in which the control has something to say.
    CHECK(read.code.has_value());
    if (read.code.has_value())
        CHECK(FastCache::Testing::Unwrap(read.code) == FastCache::NetErrorCode::Cancelled);

    // The aborted completion still arrives and must land on a live block -- #465's
    // guarantee, which the fix may not trade away for this one.
    DrainCompletions(reactor);
}

// **A SECOND defect, found while building the case above and fixed in the same change,
// with its own cases so that neither one's verdict is read as evidence about the
// other.**
//
// **What was wrong.** `IocpReactor` read the operation's error from
// `completion->overlapped.Internal`, which is an **NTSTATUS**, and handed it to every
// `IocpCompletion::dispatch` consumer as a bare `DWORD`. `MakeWsaError` then matched it
// against `TranslateWsa`, whose rows are Win32/WSA codes: `ERROR_OPERATION_ABORTED` is
// 995 and `WSAEINTR` is 10004, while a cancelled operation actually arrives as
// `STATUS_CANCELLED`, 0xC0000120. The two numbering spaces share no values, so EVERY
// IOCP error -- not merely cancellation -- fell through to `SystemError`. Measured
// before the fix: `code` was `SystemError` and `systemCode` was -1073741536.
//
// `NetErrorCode::Cancelled`'s own doc comment names *"IOCP CancelIoEx"* as the case it
// exists for. This platform had never once produced it.
//
// **This tree already knew, at one call site.** `IocpConnector::OnConnectComplete`
// carried the mechanism in its own comment and converted with `WSAGetOverlappedResult`.
// A contract answered at one consumer is one no other consumer can see, and the
// consumer that did not see it carries every read and write this product does on
// Windows. The fix is therefore the TYPE -- `IocpStatus`, which no `WSAE*` table
// accepts -- rather than a third copy of the conversion.
//
// **Two cases, because there are two branches and only one of them is the interesting
// one.** `Detail::WsaErrorOf` answers `ERROR_OPERATION_ABORTED` outright when the
// socket has already been closed, and asks `WSAGetOverlappedResult` otherwise. A case
// set that only ever reached the first branch would stay green with
// `WSAGetOverlappedResult` deleted, which is a guard nobody has watched accept. The
// reset case below is what reaches the second.
//
// **Why the aborted case closes rather than cancels.** It used to spell this with
// `CancelRead`, which was the shortest route to the observation -- and #884's fix
// completes that waiter inline and drops the late completion, so the case would have
// gone on passing while testing nothing at all. `Close()` keeps it on the completion
// path, which is the path this defect is about.
TEST_CASE("An aborted IOCP read is reported as Cancelled", "[net][iocp][socket][ntstatus]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
    std::array<std::byte, 64> buf {};
    ReadOutcome read;

    ParkOnRead(&sock, &buf, &read);
    REQUIRE_FALSE(read.resumed);

    // Closing is what aborts the pending `WSARecv`, and it is also what takes the
    // socket away from `WSAGetOverlappedResult` -- both halves of the branch under test.
    sock.Close();
    DrainCompletions(reactor);

    REQUIRE(read.resumed);
    REQUIRE(read.code.has_value());

    // Both halves, because they are two claims. `code` is what callers branch on;
    // `systemCode` is what says the raw value was CONVERTED rather than that the table
    // grew a row for an NTSTATUS -- without it, storing the NTSTATUS in `systemCode`
    // and hand-mapping the enum would pass.
    CAPTURE(read.systemCode);
    CHECK(FastCache::Testing::Unwrap(read.code) == FastCache::NetErrorCode::Cancelled);
    CHECK(read.systemCode == static_cast<int>(ERROR_OPERATION_ABORTED));
}

// The second branch, and the one that keeps `WSAGetOverlappedResult` load-bearing.
//
// A reset is not a cancellation: it arrives as `STATUS_CONNECTION_RESET` (0xC000020D)
// on a socket that is still open, so `Detail::WsaErrorOf` cannot take its
// already-closed shortcut and has to ask Winsock. Before the fix this reported
// `SystemError` too -- the reach of the defect is every IOCP error, and a case set
// that only showed cancellation would leave that as an assertion rather than a
// measurement.
//
// `SO_LINGER` with a zero timeout is what makes `closesocket` send an RST instead of a
// FIN; a FIN would complete the read with zero bytes, which is EOF and a different
// answer entirely. `pair.client` is then surrendered so `~RawPair` does not close a
// handle this case already closed.
TEST_CASE("A reset IOCP read is reported as ConnReset, not Cancelled", "[net][iocp][socket][ntstatus]")
{
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
    std::array<std::byte, 64> buf {};
    ReadOutcome read;

    ParkOnRead(&sock, &buf, &read);
    REQUIRE_FALSE(read.resumed);

    ::linger const abortive { .l_onoff = 1, .l_linger = 0 };
    REQUIRE(
        ::setsockopt(
            pair.client, SOL_SOCKET, SO_LINGER, reinterpret_cast<char const*>(&abortive), static_cast<int>(sizeof(abortive)))
        == 0);
    REQUIRE(::closesocket(pair.client) == 0);
    pair.client = INVALID_SOCKET;

    DrainCompletions(reactor);

    REQUIRE(read.resumed);
    REQUIRE(read.code.has_value());

    // Named separately from the aborted case, because "an IOCP error is translated at
    // all" and "it is translated to the RIGHT thing" are different claims, and one row
    // of a table covering for the rest is exactly how the first would pass alone.
    CAPTURE(read.systemCode);
    CHECK(FastCache::Testing::Unwrap(read.code) == FastCache::NetErrorCode::ConnReset);
    CHECK(read.systemCode == WSAECONNRESET);
}

// #884, the ticket's own acceptance item 1: EXHIBIT the reuse rather than argue it.
//
// **This one deliberately DOES double-arm**, which is what the case above refuses to do
// -- so the two are different claims and neither implies the other. The one above
// asserts the CONTRACT (retirement is synchronous); this one exhibits the CONSEQUENCE
// of the contract being broken, and can only run where `assert` is compiled out.
//
// **Why it is release-only, stated rather than left to be discovered.** `IocpSocket::Read`
// calls `Detail::ClaimReadSlot` two lines before it touches the `OVERLAPPED`, and
// `CancelRead` leaves `op.awaitable` set until `Dispatch` clears it on a later turn. So
// in a build where `assert` is live the second read aborts on #663's tripwire -- in the
// right file, two lines from the right site, for a DIFFERENT defect -- and that abort
// reads exactly like a successful reproduction of this one. Skipped rather than absent,
// so "did not run here" is a state somebody can see.
//
// **What is observed, precisely, and what turned out NOT to be.** Not the `OVERLAPPED`
// bytes, which no public interface exposes, but what reusing them does to the parked
// coroutines. The second `Read` overwrites `readOp.awaitable` while the kernel still
// owns the first operation's `OVERLAPPED`, so when the aborted completion for the first
// `WSARecv` is dequeued, `Dispatch` finds the SECOND read's awaitable in the slot -- and
// the first coroutine is never resumed and never freed. That is the leak `CancelRead`
// exists to prevent, and it is what this case is red on.
//
// **And the second read is answered with a spurious EOF**, which is worse than the
// consequence this case was first written to predict. `IocpReactor` reads the operation's
// error from `completion->overlapped.Internal` (`IocpReactor.cpp`, the socket-completion
// arm) -- a field of the very `OVERLAPPED` that `IocpSocket::Read` clears with
// `op.completion.overlapped = OVERLAPPED {}`. So the second `Read` does not merely
// reuse the block, it ERASES the `STATUS_CANCELLED` the kernel wrote into it, and the
// abort is then dispatched as `err == 0`, `bytes == 0`: a SUCCESSFUL read of zero bytes.
// On a stream socket that means the peer has finished sending -- the one false claim
// `ISocket::Read`'s contract exists to prevent, made to a reader whose two bytes are
// sitting unread on the socket, whose own completion is then dropped because the slot
// has already been cleared.
//
// The original prediction was that the second read would report `Cancelled`. It was
// measured and it does not: it reports EOF. That is recorded rather than quietly
// deleted, because the difference between a comment describing a run and a comment
// describing an argument is exactly this sentence, and `CancelRead retires a parked
// read before it returns` carries the control that attributes the zero to the erased
// status rather than to cancellation in general.
//
// The peer's send is also what keeps this artefact honest about its own teardown:
// without it the second `WSARecv` is still pending when the socket is destroyed, with
// `Op::inFlight` already released by the first completion, so the kernel would write
// into a freed block and the case would end in a use-after-free rather than a
// reportable failure.
TEST_CASE("A read armed in the same turn as CancelRead orphans the cancelled one", "[net][iocp][socket][cancelread]")
{
    #if !defined(NDEBUG)
    SKIP("release-only: with `assert` live, #663's double-arm tripwire fires before the reuse is "
         "reachable, and its abort is indistinguishable from this reproduction");
    #else
    FastCache::Detail::EnsureNetworkInitialised();
    FastCache::SteadyClock clock;
    FastCache::IocpReactor reactor { clock };
    RawPair pair;

    FastCache::IocpSocket sock { reactor, static_cast<std::uintptr_t>(pair.served) };
    std::array<std::byte, 64> firstBuf {};
    std::array<std::byte, 64> secondBuf {};
    ReadOutcome first;
    ReadOutcome second;

    ParkOnRead(&sock, &firstBuf, &first);
    REQUIRE_FALSE(first.resumed);

    sock.CancelRead();

    // The double-arm. Same reactor turn: nothing has drained the port, so the aborted
    // completion for the first `WSARecv` cannot have been dequeued yet.
    ParkOnRead(&sock, &secondBuf, &second);
    REQUIRE_FALSE(second.resumed);

    // Only now is there anything for the second read to be answered WITH.
    static constexpr std::size_t PayloadSize = 2;
    std::array<char, PayloadSize> const payload { 'f', 'c' };
    REQUIRE(::send(pair.client, payload.data(), static_cast<int>(payload.size()), 0) == static_cast<int>(PayloadSize));

    DrainCompletions(reactor);

    // **The spurious EOF.** `second.resumed` and the disengaged `code` both pass under
    // the defect -- the read "succeeded" -- and the byte count is what tells the two
    // states apart: 0 where its own operation had 2 waiting. Asserting only that the
    // read succeeded would be a guard that cannot fail for the reason it exists.
    CHECK(second.resumed);
    CHECK_FALSE(second.code.has_value());
    CHECK(second.bytes == PayloadSize);

    // **The first read's coroutine is the casualty, and this is the red half.** Under
    // the defect it is never resumed: the second `Read` overwrote `readOp.awaitable`
    // before the aborted completion for the first `WSARecv` was dequeued, so `Dispatch`
    // had nobody to hand the abort to and the first coroutine's frame is parked
    // forever. Deliberately `CHECK` rather than `REQUIRE`, so both lines report and the
    // failure names the orphan AND the missing reason rather than only the first of
    // them.
    CHECK(first.resumed);
    CHECK(first.code.has_value());
    if (first.code.has_value())
        CHECK(FastCache::Testing::Unwrap(first.code) == FastCache::NetErrorCode::Cancelled);
    #endif
}

#endif // _WIN32
