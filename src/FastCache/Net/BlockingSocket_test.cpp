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
#include <FastCache/Net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#if !defined(_WIN32)
    #include <sys/socket.h>

    #include <csignal>
#endif

#include <array>
#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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

TEST_CASE("An armed listener's Accept wakes on its own poll, with nobody connecting", "[net][socket][listener]")
{
    // #260's portability half. The admin accept loop wakes to re-check a shutdown
    // flag, and the wake used to be `SO_RCVTIMEO` on the LISTENING socket -- which
    // Linux honours for `accept()` and macOS and the BSDs do NOT. There the accept
    // blocked forever, so a teardown that waits for the loop to leave hung until
    // the harness killed it: three fleet-dashboard e2e cases timed out on macOS and
    // passed everywhere else.
    //
    // This asserts the mechanism rather than a teardown, because a teardown test
    // passes on Linux under the bug -- the platform that has the bug is the one that
    // cannot run this suite locally.
    auto listener = BindEphemeral();
    if (listener == nullptr)
        SKIP("this platform would not bind a loopback listener");
    listener->SetTimeouts(150ms, 1000ms);

    auto const startedAt = std::chrono::steady_clock::now();
    auto const accepted = SyncRun(AcceptOne(listener.get()));
    auto const elapsed = std::chrono::steady_clock::now() - startedAt;

    // It must REPORT rather than block, and it must report the state the accept
    // loop steps over rather than a real failure.
    REQUIRE_FALSE(accepted.has_value());
    CHECK(IsDeadlineExpiry(accepted.error().code));

    // Bounded on BOTH sides. Too fast means it did not wait at all -- an accept that
    // returns instantly would spin the loop -- and too slow means the poll was not
    // what returned it. Generous on the upper side: a loaded CI runner is slow, and
    // what is being separated here is 150ms from FOREVER.
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 10s);
}

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
        SKIP("no loopback listener available on this host");

    BlockingConnector connector;
    auto client = SyncRun(connector.Connect("127.0.0.1", listener->BoundPort(), DialOptions { .connectTimeout = 2s }));
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
namespace
{

/// Read the process-wide SIGPIPE disposition.
/// @return The installed handler, or `SIG_ERR` when it could not be read.
[[nodiscard]] auto ReadSigPipeDisposition() noexcept -> void (*)(int)
{
    struct sigaction current {};
    if (::sigaction(SIGPIPE, nullptr, &current) != 0)
        return SIG_ERR;
    return current.sa_handler;
}

/// Name a SIGPIPE disposition, so a failure prints what it saw rather than a pointer.
/// @param disposition The handler to name.
/// @return A readable name for it.
[[nodiscard]] std::string_view NameSigPipeDisposition(void (*disposition)(int))
{
    if (disposition == SIG_DFL)
        return "SIG_DFL";
    if (disposition == SIG_IGN)
        return "SIG_IGN";
    if (disposition == SIG_ERR)
        return "SIG_ERR -- the disposition could not be read at all";
    return "a handler installed by something in this process";
}

/// The SIGPIPE disposition this process was HANDED, read before any case runs.
///
/// The case below asserts a DELTA against this, where it used to assert the
/// absolute value `SIG_DFL` -- and the difference is the very thing the case is
/// about. An ignored disposition is inherited across fork AND exec, which is the
/// sentence this case exists to keep true, so the absolute form is a claim about
/// the whole process ANCESTRY rather than about this library: a parent that
/// ignores SIGPIPE hands it down, and the assertion then reads `SIG_IGN` with
/// nothing in this tree having touched a signal.
///
/// Not hypothetical, and not equally reachable from every leg. #1229 was this
/// case failing `clang-tsan` while passing four other Linux legs on the same
/// commit: the ThreadSanitizer gate execs this binary straight from its step's
/// shell and inherits whatever that process tree holds, while every other leg
/// reaches the case through ctest, which spawns each case with libuv and resets
/// every disposition to `SIG_DFL` in the child. Measured in both directions on
/// one host, one case, no ordering involved: from a parent that ignores SIGPIPE
/// a direct exec of this binary fails `0x1 == nullptr` -- byte for byte what CI
/// reported -- and the same case launched by ctest from that same parent passes.
///
/// Read at static-initialisation time, which is before any case runs. Order
/// across translation units is unspecified, so what this cannot see is another
/// static initialiser arming SIGPIPE first. Nothing in `src/` does: the only
/// `SIG_IGN` in the tree is `ArmNoSigPipe`'s `#else` arm, which Linux never
/// compiles because `MSG_NOSIGNAL` is defined.
auto const InheritedSigPipeDisposition = ReadSigPipeDisposition();

} // namespace

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
    //
    // Asserted as a DELTA and not as the absolute `SIG_DFL` it used to be. The
    // reasoning is on `InheritedSigPipeDisposition` above, and it is the same
    // inheritance the paragraph above describes, pointed at this case.
    auto const before = ReadSigPipeDisposition();
    REQUIRE(before != SIG_ERR);

    auto listener = BindEphemeral();
    if (listener == nullptr)
        SKIP("no loopback listener available on this host");

    BlockingConnector connector;
    auto client = SyncRun(connector.Connect("127.0.0.1", listener->BoundPort(), DialOptions { .connectTimeout = 2s }));
    REQUIRE(client.has_value());

    auto const after = ReadSigPipeDisposition();
    REQUIRE(after != SIG_ERR);

    INFO("handed to this process: " << NameSigPipeDisposition(InheritedSigPipeDisposition));
    INFO("before this case touched a socket: " << NameSigPipeDisposition(before));
    INFO("after: " << NameSigPipeDisposition(after));

    // TWO questions, repaired in two different places, so two CHECKs rather than
    // one. `after == before` says this case's own socket use changed nothing --
    // the case's literal title. `after == InheritedSigPipeDisposition` says
    // nothing ANYWHERE in this process has, which is the stronger claim and is
    // only askable here: under ctest every case is its own process, so an arm by
    // an earlier case is invisible, while the ThreadSanitizer gate runs the whole
    // tag expression in ONE process, where it is not.
    CHECK(after == before);
    CHECK(after == InheritedSigPipeDisposition);

    // What neither CHECK can see is a process that was handed `SIG_IGN` to begin
    // with: an arm by this library would then change nothing, and both pass. So
    // that environment is named OUT LOUD instead of passing quietly -- a green
    // case there has not established what a green case elsewhere establishes, and
    // those are two states one `passed` collapses into one.
    if (InheritedSigPipeDisposition != SIG_DFL)
        WARN("this process was handed SIGPIPE = " << NameSigPipeDisposition(InheritedSigPipeDisposition)
                                                  << ", so this case could only assert that nothing changed the "
                                                     "disposition, not that it is SIG_DFL. See #1229.");
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
        SKIP("no loopback listener available on this host");

    BlockingConnector connector;
    auto client = SyncRun(connector.Connect("127.0.0.1", listener->BoundPort(), DialOptions { .connectTimeout = 2s }));
    REQUIRE(client.has_value());

    auto accepted = SyncRun(AcceptOne(listener.get()));
    REQUIRE(accepted.has_value());

    std::array<std::byte, 4> const payload { std::byte { 1 }, std::byte { 2 }, std::byte { 3 }, std::byte { 4 } };
    auto const written = SyncRun(WriteOnce(client->get(), std::span<std::byte const> { payload }));
    REQUIRE(written.has_value());
    CHECK(*written == payload.size());
}

#if !defined(_WIN32)
TEST_CASE("Suppression is either armed on the socket or carried in the send flags", "[net][socket]")
{
    // The two halves of per-socket SIGPIPE suppression are one rule, and which
    // half does the work flips between platforms: `SO_NOSIGPIPE` arms the
    // descriptor on macOS and the BSDs, while on Linux there is nothing to arm and
    // `MSG_NOSIGNAL` on every send is the whole of the protection.
    //
    // So a raw sender that arms and then passes `0` is correct on one platform and
    // fatally wrong on the other -- and it reads as correct on both, because the
    // arming call is right there. `HealthProbe` did exactly that: on Linux
    // `fastcached --healthcheck` against a peer that hung up mid-request died of
    // signal 13 instead of reporting the peer unhealthy, which for a Docker
    // HEALTHCHECK or a systemd probe is the one answer a health check must be able
    // to give.
    //
    // Asserted against the platform macro rather than through a hung-up peer,
    // because the consequence is not reproducible on demand: the probe sends one
    // small request, and a single small write after a hang-up is routinely
    // *accepted* -- it takes the peer's RST and a second write to break the pipe,
    // which is why the case above needs kilobytes to provoke it. A regression test
    // that only sometimes sees the signal is the thing this repository already
    // records as worth nothing, so this pins the invariant the fix restored.
    // Both arms assert, and the `SO_NOSIGPIPE` one is not symmetry for tidiness: ZERO is
    // the answer this case exists to pin. `MSG_NOSIGNAL` may well be DEFINED on a macOS
    // SDK new enough to declare it, while `CMAKE_OSX_DEPLOYMENT_TARGET` lets the binary
    // run on an older kernel -- and a flag the kernel does not know fails the send. So
    // widening this arm to `MSG_NOSIGNAL` is the exact regression, and until #685 it
    // would have been reported as a pass: the arm said `SUCCEED("SO_NOSIGPIPE arms the
    // descriptor here")`, which observed nothing at all.
    #if defined(SO_NOSIGPIPE)
    CHECK(Detail::NoSignalSendFlags() == 0);
    #else
    CHECK(Detail::NoSignalSendFlags() == MSG_NOSIGNAL);
    #endif
}
#endif

// -- Detail::AcceptRaw / Detail::CloseNativeSocket -------------------------

TEST_CASE("closing a listening socket unblocks a parked AcceptRaw, and says which", "[net][socket][listener]")
{
    // `Detail::CloseNativeSocket`'s contract -- *"e.g. the listening socket, to unblock
    // a thread parked in AcceptRaw()"* -- and `AcceptRaw`'s from the other side, *"a
    // NetError (Cancelled-like when the listening socket was closed to unblock the
    // accept)"*. **Nothing asserted either of them.** `AcceptRaw` had no test anywhere,
    // so the one mechanism `RunMultiReactorWindows`' `stopAll` uses to end its acceptor
    // threads was a sentence in one header, checked by nobody
    // ([#1238](https://github.com/LASTRADA-Software/fastcached/issues/1238)).
    //
    // That absence is what makes the shape read as a defect. #1238 suspected
    // close-under-accept there, by analogy with
    // [#1207](https://github.com/LASTRADA-Software/fastcached/issues/1207), whose remedy
    // is *stop, JOIN, then close*. **The analogy does not transfer, and the measurements
    // are why.** #1207 rests on POSIX not waking a parked `accept()`, so closing early
    // buys nothing there; here the close is the ONLY thing that ends a thread inside a
    // blocking `::accept`, and joining first would hang forever. The POSIX sibling
    // (`RunMultiReactorPosix`) can join-then-close only because its acceptors are
    // reactor-driven `PlatformListener`s rather than threads in a bare syscall.
    //
    // So the arrangement is deliberate, and this pins it rather than changing it.
    //
    // **What makes this a test and not a smoke check is the SECOND arm.** A close that
    // merely made the accept *fail* would be satisfied by the thread never having parked
    // at all -- the accept called a moment AFTER the close, which is a different fact
    // and one the loop also produces. Measured on Windows 11 / clang-cl, 3 runs each and
    // identical every time:
    //
    //   - parked, then closed underneath -> `WSAEINTR` (10004) -> `NetErrorCode::Cancelled`
    //   - called on an already-closed handle -> `WSAENOTSOCK` (10038) -> `BadFileHandle`
    //
    // Two different codes, so the pair DISCRIMINATES. Asserting only "it returned an
    // error" would assert what both sides produce, which is no assertion at all.
    //
    // **POSIX skips, and the skip is the finding.** Measured on Linux (WSL2, glibc), 3
    // runs: a thread in `accept()` was STILL PARKED 12.5 s after `close()` on its
    // listening fd. So the contract quoted above holds on Windows and is false here --
    // which costs nothing today, because `AcceptRaw`'s only caller is inside
    // `#if defined(_WIN32)`, and the header now says so. A `SKIP` rather than a
    // `SUCCEED`: the case did not run, and reporting a pass for a property nothing
    // established is #685. It is deliberately NOT `#if`-compiled away, so the body goes
    // on being compiled on the platform the local gate runs -- a platform arm that ships
    // uncompiled is one nobody has built.
#if !defined(_WIN32)
    SKIP("close() does not unblock a parked accept() on this platform -- measured, still parked "
         "12.5s later, 3/3 on Linux -- and AcceptRaw has no caller outside _WIN32");
#endif

    // The production spelling: `RunMultiReactorWindows` binds through exactly this call
    // with exactly this resolver. A fixture that bound its own socket by hand would be
    // testing a socket, not the seam.
    auto bound = Detail::BindAndListen(DefaultAddressResolver(), "127.0.0.1", 0, /*backlog*/ 4, /*extraTypeFlags*/ 0);
    if (!bound.has_value())
        SKIP("this platform would not bind a loopback listener: " + bound.error());

    SECTION("a parked accept is cancelled, not merely failed")
    {
        auto const listenSock = bound->socket;

        // Nothing ever dials this port, so the accept genuinely parks rather than
        // racing a pending connection -- which is the arrangement `stopAll` meets.
        std::promise<NetError> outcome;
        auto answered = outcome.get_future();
        std::thread acceptor { [listenSock, &outcome] {
            auto accepted = Detail::AcceptRaw(listenSock);
            if (accepted.has_value())
            {
                Detail::CloseNativeSocket(*accepted);
                outcome.set_value(NetError { .code = NetErrorCode::Ok, .systemCode = 0, .context = "accept SUCCEEDED" });
                return;
            }
            outcome.set_value(std::move(accepted.error()));
        } };

        // Long enough that the acceptor is inside the syscall. If it were not, the close
        // would land first and the code below would read `BadFileHandle` -- which is why
        // the assertion is on WHICH error, not on the fact of one: a too-short pause
        // fails this case instead of passing it quietly.
        std::this_thread::sleep_for(250ms);
        Detail::CloseNativeSocket(listenSock);

        if (answered.wait_for(10s) != std::future_status::ready)
        {
            // Detach rather than join: the thread is in a syscall nothing remaining can
            // end, so joining would hang the whole binary instead of failing this case
            // -- and a hang under `catch_discover_tests` is a timeout that names nothing.
            acceptor.detach();
            FAIL("AcceptRaw was still parked 10s after its listening socket was closed, so the mechanism "
                 "RunMultiReactorWindows' stopAll depends on does not hold on this platform (#1238)");
        }
        acceptor.join();

        auto const err = answered.get();
        INFO("AcceptRaw returned " << err.ToString());
        CHECK(err.code == NetErrorCode::Cancelled);
    }

    SECTION("an accept CALLED after the close reports the other code")
    {
        // The control that makes the section above mean something. Same close, same
        // helper, no parked thread -- and it must NOT answer `Cancelled`, or the two
        // states this pair exists to separate are one state and the assertion is empty.
        auto const listenSock = bound->socket;
        Detail::CloseNativeSocket(listenSock);

        auto const accepted = Detail::AcceptRaw(listenSock);
        REQUIRE_FALSE(accepted.has_value());
        INFO("AcceptRaw returned " << accepted.error().ToString());
        CHECK(accepted.error().code == NetErrorCode::BadFileHandle);
        CHECK(accepted.error().code != NetErrorCode::Cancelled);
    }
}
