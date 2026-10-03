// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/AcceptPolicy.hpp>
#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/Types.hpp>
#include <core/platform/WinsockInit.hpp>
#include <tests/BoundedWait.hpp>
#include <tests/ScopedSignalHandler.hpp>
#include <tests/Unwrap.hpp>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <cerrno>
    #include <csignal>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

using namespace std::chrono_literals;
using FastCache::Testing::Unwrap;

namespace
{

/// An IPv4 candidate for `ip`:`port`, as a resolver would hand it to `BindAndListen`, so a case
/// scripts the candidate list with no dependency on real DNS.
core::net::ResolvedEndpoint MakeV4Endpoint(char const* ip, std::uint16_t port)
{
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &sa.sin_addr);

    core::net::ResolvedEndpoint endpoint;
    std::memcpy(endpoint.storage.data(), &sa, sizeof(sa));
    endpoint.length = sizeof(sa);
    endpoint.family = AF_INET;
    endpoint.protocol = IPPROTO_TCP;
    return endpoint;
}

/// An IPv6 candidate for `ip`:`port` (the "::" wildcard, typically).
core::net::ResolvedEndpoint MakeV6Endpoint(char const* ip, std::uint16_t port)
{
    sockaddr_in6 sa {};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    ::inet_pton(AF_INET6, ip, &sa.sin6_addr);

    core::net::ResolvedEndpoint endpoint;
    std::memcpy(endpoint.storage.data(), &sa, sizeof(sa));
    endpoint.length = sizeof(sa);
    endpoint.family = AF_INET6;
    endpoint.protocol = IPPROTO_TCP;
    return endpoint;
}

/// A resolver that replays a fixed candidate list or a fixed error, whatever it is asked.
class FakeAddressResolver final: public core::net::IAddressResolver
{
  public:
    explicit FakeAddressResolver(std::vector<core::net::ResolvedEndpoint> endpoints):
        _endpoints { std::move(endpoints) }
    {
    }

    explicit FakeAddressResolver(std::string error):
        _error { std::move(error) }
    {
    }

    [[nodiscard]] std::expected<std::vector<core::net::ResolvedEndpoint>, std::string> resolve(
        std::string_view /*host*/, std::uint16_t /*port*/) override
    {
        if (!_error.empty())
            return std::unexpected(_error);
        return _endpoints;
    }

  private:
    std::vector<core::net::ResolvedEndpoint> _endpoints;
    std::string _error;
};

/// Bind loopback through the production sequence.
/// @param port The port; 0 lets the kernel choose.
/// @return The bound socket, or why not.
[[nodiscard]] std::expected<FastCache::BoundSocket, std::string> BindLoopback(std::uint16_t port)
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("127.0.0.1", port) } };
    return FastCache::BindAndListen(resolver, "127.0.0.1", port, /*backlog*/ 16);
}

/// Await one accept.
/// @param listener The listener; a pointer, since a coroutine's reference parameter can dangle.
/// @return The accepted socket, or the accept error.
[[nodiscard]] core::async::Task<core::net::AcceptResult> AcceptOne(FastCache::BlockingListener* listener)
{
    co_return co_await listener->accept();
}

} // namespace

TEST_CASE("BindAndListen binds the first resolved candidate", "[net][bind]")
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("127.0.0.1", 0) } };
    auto bound = FastCache::BindAndListen(resolver, "127.0.0.1", 0, /*backlog*/ 16);
    REQUIRE(bound.has_value());
    REQUIRE(bound->handle != core::platform::InvalidHandle);
    REQUIRE(bound->family == AF_INET);
    FastCache::CloseNativeSocket(bound->handle);
}

TEST_CASE("BindAndListen falls over to the next candidate when the first cannot bind", "[net][bind]")
{
    // 192.0.2.1 is TEST-NET-1 (RFC 5737): never assigned to a local interface, so bind() fails
    // with EADDRNOTAVAIL and the loop tries the next candidate.
    FakeAddressResolver resolver { std::vector {
        MakeV4Endpoint("192.0.2.1", 0),
        MakeV4Endpoint("127.0.0.1", 0),
    } };
    auto bound = FastCache::BindAndListen(resolver, "ignored", 0, /*backlog*/ 16);
    REQUIRE(bound.has_value());
    REQUIRE(bound->family == AF_INET);
    FastCache::CloseNativeSocket(bound->handle);
}

TEST_CASE("BindAndListen propagates a resolver error", "[net][bind]")
{
    FakeAddressResolver resolver { std::string { "cannot resolve 'banana': no usable address" } };
    auto const bound = FastCache::BindAndListen(resolver, "banana", 11211, /*backlog*/ 16);
    REQUIRE_FALSE(bound.has_value());
    REQUIRE(bound.error().contains("banana"));
}

TEST_CASE("BindAndListen reports failure when no candidate is bindable", "[net][bind]")
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("192.0.2.1", 0) } };
    auto const bound = FastCache::BindAndListen(resolver, "192.0.2.1", 0, /*backlog*/ 16);
    REQUIRE_FALSE(bound.has_value());
}

TEST_CASE("BindAndListen keeps its address to itself while it is listening", "[net][bind][exclusive]")
{
    // A listening address is not shareable, and saying so takes a different option on each
    // platform. POSIX SO_REUSEADDR only lets a bind step over a TIME_WAIT left by a DEAD socket;
    // Windows SO_REUSEADDR lets a second socket bind an address a LIVE one already holds -- so
    // setting it there let any process on the box take the port fastcached was already serving
    // (#85). Both sides go through BindAndListen: that is the production shape, and it is the
    // option BindAndListen sets that has to refuse it.
    auto held = BindLoopback(0);
    REQUIRE(held.has_value());

    auto const port = FastCache::BoundPortOf(held->handle);
    REQUIRE(port != 0);

    auto const taken = BindLoopback(port);
    // CHECK rather than REQUIRE so the cleanup below still runs when this regresses.
    CHECK_FALSE(taken.has_value());
    if (taken.has_value())
        FastCache::CloseNativeSocket(taken->handle);

    FastCache::CloseNativeSocket(held->handle);
}

TEST_CASE("ClientListenOptions asks for 1 MiB buffers each way, whatever the sharing", "[net][listener][buffers]")
{
    // Both daemon paths bind through this one value, so a reply of up to 1 MiB leaves in one write
    // on either. The single-reactor path used to call `core::net::listen` with no buffers at all,
    // which kept the kernel's default on every connection it accepted.
    auto const sharing = GENERATE(core::net::PortSharing::Exclusive, core::net::PortSharing::Shared);
    auto const options = FastCache::ClientListenOptions("127.0.0.1", 11211, 64, sharing);
    CHECK(options.host == "127.0.0.1");
    CHECK(options.port == 11211);
    CHECK(options.backlog == 64);
    CHECK(options.sharing == sharing);
    CHECK(options.buffers.send == FastCache::ClientSocketBufferBytes);
    CHECK(options.buffers.receive == FastCache::ClientSocketBufferBytes);
    CHECK(FastCache::ClientSocketBufferBytes == std::size_t { 1 } << 20);
}

TEST_CASE("PortSharing::Shared lets several loops' listeners bind one port", "[net][listener][reuseport]")
{
    // Exclusivity is the default, not the only setting. The POSIX multi-reactor binds one
    // listener per loop on the same {address, port} and lets the kernel spread connections
    // across them, so a change that made every bind exclusive would present as a daemon that
    // refuses to start with more than one reactor thread. Windows has no such option, and
    // core-cpp refuses it there rather than map it onto a hijackable SO_REUSEADDR.
    core::net::PlatformLoop first;
    core::net::PlatformLoop second;
    auto held = core::net::listen(first, FastCache::ClientListenOptions("127.0.0.1", 0, 16, core::net::PortSharing::Shared));
#if defined(_WIN32)
    REQUIRE_FALSE(held.has_value());
    CHECK(held.error().code == core::net::NetErrorCode::Unsupported);
#else
    REQUIRE(held.has_value());
    auto const port = (*held)->boundPort();
    REQUIRE(port != 0);

    auto shared =
        core::net::listen(second, FastCache::ClientListenOptions("127.0.0.1", port, 16, core::net::PortSharing::Shared));
    CHECK(shared.has_value());

    // And exclusive is still exclusive: a third listener that does not ask to share is refused.
    auto exclusive =
        core::net::listen(second, FastCache::ClientListenOptions("127.0.0.1", port, 16, core::net::PortSharing::Exclusive));
    CHECK_FALSE(exclusive.has_value());
#endif
}

TEST_CASE("BindAndListen forces dual-stack (IPV6_V6ONLY=0) on an IPv6 wildcard bind", "[net][bind][dual-stack]")
{
    FakeAddressResolver resolver { std::vector { MakeV6Endpoint("::", 0) } };
    auto bound = FastCache::BindAndListen(resolver, "::", 0, /*backlog*/ 16);
    if (!bound.has_value())
        SKIP("IPv6 unavailable in this environment, so the dual-stack bind could not be made");
    REQUIRE(bound->family == AF_INET6);

    int v6only = 1;
#if defined(_WIN32)
    int len = sizeof(v6only);
    auto const rc = ::getsockopt(
        reinterpret_cast<SOCKET>(bound->handle), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &len);
#else
    socklen_t len = sizeof(v6only);
    auto const rc = ::getsockopt(bound->handle, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &len);
#endif
    REQUIRE(rc == 0);
    REQUIRE(v6only == 0); // dual-stack: the "::" socket also accepts IPv4 clients
    FastCache::CloseNativeSocket(bound->handle);
}

TEST_CASE("BoundPortOf reports the port the kernel chose, not the one asked for", "[net][listener]")
{
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("cannot bind loopback here");

    auto const port = listener->boundPort();
    CHECK(port != 0);
    // And it is stable: the question is about the socket, not about the call.
    CHECK(listener->boundPort() == port);
}

TEST_CASE("A released listening socket is served by a loop on the port it held, never freed in between",
          "[net][listener][adopt]")
{
    auto held = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(held != nullptr);
    if (!held->IsBound())
        SKIP("cannot bind loopback here");
    auto const port = held->boundPort();

    // Its own type, not `auto`: a pointer on Windows and an int on POSIX.
    core::platform::NativeHandle const handle = held->Release();
    REQUIRE(handle != core::platform::InvalidHandle);
    CHECK_FALSE(held->IsBound());
    // Released means the destructor no longer owns it: were it closed here, the adoption below
    // would be refused, and the port free for anyone.
    held.reset();

    core::net::PlatformLoop loop;
    auto adopted = FastCache::AdoptBoundListener(loop, handle);
    REQUIRE(adopted.has_value());
    CHECK((*adopted)->boundPort() == port);

    // Still claimed, by the adopted listener: an exclusive bind of the same port is refused.
    auto const second = FastCache::BlockingListener::Bind("127.0.0.1", port);
    REQUIRE(second != nullptr);
    CHECK_FALSE(second->IsBound());
}

TEST_CASE("AdoptBoundListener refuses no socket at all, before touching anything", "[net][listener][adopt]")
{
    core::net::PlatformLoop loop;
    auto const adopted = FastCache::AdoptBoundListener(loop, core::platform::InvalidHandle);
    REQUIRE_FALSE(adopted.has_value());
    CHECK(adopted.error() == "adopt: not a socket");
}

TEST_CASE("BoundPortOf reports 0 for a handle that is not bound", "[net][listener]")
{
    CHECK(FastCache::BoundPortOf(core::platform::InvalidHandle) == 0);
}

TEST_CASE("BoundAddressOf reports the host the kernel bound, in the canonical literal spelling", "[net][listener]")
{
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("cannot bind loopback here");
    CHECK(listener->BoundAddress() == "127.0.0.1");
    CHECK(FastCache::BoundAddressOf(core::platform::InvalidHandle).empty());
}

TEST_CASE("CanonicalAddressLiteral spells one address one way, and a name as nothing", "[net][listener]")
{
    // The spelling `BoundAddressOf` reports, so a configured literal compares with the kernel's
    // answer however it was written.
    CHECK(FastCache::CanonicalAddressLiteral("127.0.0.1") == "127.0.0.1");
    CHECK(FastCache::CanonicalAddressLiteral("::0001") == "::1");
    CHECK(FastCache::CanonicalAddressLiteral("0:0:0:0:0:0:0:1") == "::1");
    CHECK(FastCache::CanonicalAddressLiteral("0.0.0.0") == "0.0.0.0");
    // Pure: a name is not looked up, so it is not a literal -- and neither is a bracketed one,
    // since the contract is unbracketed text.
    CHECK_FALSE(FastCache::CanonicalAddressLiteral("localhost").has_value());
    CHECK_FALSE(FastCache::CanonicalAddressLiteral("[::1]").has_value());
    CHECK_FALSE(FastCache::CanonicalAddressLiteral("").has_value());
}

TEST_CASE("BoundEndpointOfDescriptor reads the address and port off a descriptor number as a supervisor hands one over",
          "[net][listener][activation]")
{
    CHECK_FALSE(FastCache::BoundEndpointOfDescriptor(-1).has_value());
#if defined(_WIN32)
    // No socket activation: a number names no socket, so there is never an endpoint to read.
    CHECK_FALSE(FastCache::BoundEndpointOfDescriptor(3).has_value());
#else
    // One address: the socket names it, not the wildcard.
    auto held = BindLoopback(0);
    REQUIRE(held.has_value());
    auto const port = FastCache::BoundPortOf(held->handle);
    REQUIRE(port != 0);
    // The same socket, named the way an activation handoff names it.
    auto const bound = FastCache::BoundEndpointOfDescriptor(static_cast<int>(held->handle));
    REQUIRE(bound.has_value());
    CHECK(FastCache::Testing::Unwrap(bound).host == "127.0.0.1");
    CHECK(FastCache::Testing::Unwrap(bound).port == port);
    FastCache::CloseNativeSocket(held->handle);

    // The wildcard: a bare `ListenStream=<port>` binds every interface, and the socket says so.
    FakeAddressResolver anywhere { std::vector { MakeV4Endpoint("0.0.0.0", 0) } };
    auto wildcard = FastCache::BindAndListen(anywhere, "0.0.0.0", 0, /*backlog*/ 16);
    REQUIRE(wildcard.has_value());
    auto const any = FastCache::BoundEndpointOfDescriptor(static_cast<int>(wildcard->handle));
    REQUIRE(any.has_value());
    CHECK(FastCache::Testing::Unwrap(any).host == "0.0.0.0");
    CHECK(FastCache::Testing::Unwrap(any).port == FastCache::BoundPortOf(wildcard->handle));
    FastCache::CloseNativeSocket(wildcard->handle);
#endif
}

TEST_CASE("An armed listener's accept wakes on its own poll, with nobody connecting", "[net][socket][listener]")
{
    // #260's portability half. The admin accept loop wakes to re-check a shutdown flag, and the
    // wake used to be `SO_RCVTIMEO` on the LISTENING socket -- which Linux honours for `accept()`
    // and macOS and the BSDs do NOT. This asserts the mechanism rather than a teardown, because a
    // teardown test passes on Linux under the bug.
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("this platform would not bind a loopback listener");
    listener->SetTimeouts(150ms, 1000ms);

    auto const startedAt = std::chrono::steady_clock::now();
    auto const accepted = core::async::syncRun(AcceptOne(listener.get()));
    auto const elapsed = std::chrono::steady_clock::now() - startedAt;

    // It must REPORT rather than block, and report the state the accept loop steps over.
    REQUIRE_FALSE(accepted.has_value());
    CHECK(core::net::isDeadlineExpiry(accepted.error().code));

    // Bounded on BOTH sides: too fast means it did not wait at all -- an accept that returns
    // instantly would spin the loop -- and too slow means the poll was not what returned it.
    CHECK(elapsed >= 100ms);
    CHECK(elapsed < 10s);
}

TEST_CASE("A socket this process owns is not handed to the children it spawns", "[net][listener]")
{
    // `fastcache-compile-node` accepts connections and spawns a compiler for every job, so a
    // socket a child inherits is a client connection that stays open for as long as an unrelated
    // compile runs. `ApplyHotSocketOptions` is what every socket `BlockingListener` accepts
    // passes through.
    core::platform::ensureWinsockInitialized();

#if defined(_WIN32)
    auto const raw = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(std::cmp_not_equal(raw, INVALID_SOCKET));
    auto* const native = reinterpret_cast<core::platform::NativeHandle>(raw);

    // The default is asserted too: a Windows socket IS inheritable unless something says
    // otherwise.
    DWORD before = 0;
    auto* const handle = reinterpret_cast<HANDLE>(raw);
    REQUIRE(::GetHandleInformation(handle, &before) != 0);
    CHECK((before & HANDLE_FLAG_INHERIT) != 0);

    FastCache::ApplyHotSocketOptions(native);

    DWORD after = 0;
    REQUIRE(::GetHandleInformation(handle, &after) != 0);
    CHECK((after & HANDLE_FLAG_INHERIT) == 0);
#else
    auto const native = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(native >= 0);

    // Same on POSIX: a plain `::socket` is NOT close-on-exec.
    REQUIRE(::fcntl(native, F_GETFD) >= 0);
    CHECK((::fcntl(native, F_GETFD) & FD_CLOEXEC) == 0);

    FastCache::ApplyHotSocketOptions(native);

    CHECK((::fcntl(native, F_GETFD) & FD_CLOEXEC) != 0);
#endif

    FastCache::CloseNativeSocket(native);
}

namespace
{

/// The pause `CloseUnderParkedAccept` is given on the first attempt, doubled on every attempt after it.
constexpr auto FirstParkPause = 50ms;

/// How many attempts may try to catch an accept parked: pauses of 50 ms up to 1.6 s, 3.15 s at most.
constexpr auto ParkAttempts = 6;

/// Start a thread in `AcceptRaw` on @p listening, close @p listening under it, and report what the accept said.
///
/// The thread says when it is about to make the call and the close comes @p pause after THAT, so a thread slow
/// to start costs nothing. What no signal can say is that it is INSIDE the syscall -- a blocking `::accept` has
/// no entry this process can observe, so a hook in `AcceptRaw` could only say "about to call" as well, which is
/// why none is added there. The caller judges each attempt by the answer instead.
///
/// The thread's state is shared rather than borrowed: an accept that never returns is detached, and a detached
/// thread that did return later must not write into a frame that has gone.
/// @param listening A bound, listening socket; closed by this call.
/// @param pause How long after the thread says it is about to call to close the socket.
/// @return What `AcceptRaw` returned, or nullopt when it was still parked 10 s after the close.
[[nodiscard]] std::optional<core::net::NetError> CloseUnderParkedAccept(core::platform::NativeHandle listening,
                                                                        std::chrono::milliseconds pause)
{
    struct Shared
    {
        std::promise<void> calling;
        std::promise<core::net::NetError> outcome;
    };
    auto const shared = std::make_shared<Shared>();
    auto aboutToCall = shared->calling.get_future();
    auto answered = shared->outcome.get_future();
    std::thread acceptor { [listening, shared] {
        shared->calling.set_value();
        auto accepted = FastCache::AcceptRaw(listening);
        if (accepted.has_value())
        {
            FastCache::CloseNativeSocket(accepted->handle);
            shared->outcome.set_value(core::net::makeNetError(core::net::NetErrorCode::Ok, 0, "accept SUCCEEDED"));
            return;
        }
        shared->outcome.set_value(std::move(accepted).error());
    } };

    // Bounded like every wait here; a thread that has not started by then makes this attempt read `BadHandle`,
    // which the caller treats as proving nothing.
    (void) aboutToCall.wait_for(10s);
    std::this_thread::sleep_for(pause);
    FastCache::CloseNativeSocket(listening);

    if (answered.wait_for(10s) != std::future_status::ready)
    {
        // Detach rather than join: the thread is in a syscall nothing remaining can end.
        acceptor.detach();
        return std::nullopt;
    }
    acceptor.join();
    return answered.get();
}

} // namespace

TEST_CASE("closing a listening socket unblocks a parked AcceptRaw, and says which", "[net][socket][listener]")
{
    // The one mechanism `RunMultiReactorWindows`' `stopAll` uses to end its acceptor threads:
    // closing the listening socket under a thread parked in `AcceptRaw` (#1238). What makes this
    // a test and not a smoke check is the SECOND arm. A close that merely made the accept FAIL
    // would be satisfied by the thread never having parked at all -- the accept called a moment
    // AFTER the close, which is a different fact and one the loop also produces. Measured on
    // Windows 11 / clang-cl, 3 runs each:
    //
    //   - parked, then closed underneath -> `WSAEINTR` (10004) -> `NetErrorCode::Cancelled`
    //   - called on an already-closed handle -> `WSAENOTSOCK` (10038) -> `NetErrorCode::BadHandle`
    //
    // **POSIX skips, and the skip is the finding**: a thread in `accept()` was still parked past a
    // 12 s ceiling after `close()` on its listening fd (`scripts/probes/accept-close-wakeup.cpp`),
    // so the contract holds on Windows only -- where `AcceptRaw`'s one caller is. Not compiled
    // away, so the body goes on building on the platform the local gate runs.
#if !defined(_WIN32)
    SKIP("close() does not unblock a parked accept() on this platform -- measured, still parked past a 12 s ceiling "
         "(12002-12045 ms observed), 3/3 on Linux, by scripts/probes/accept-close-wakeup.cpp -- and AcceptRaw has no "
         "caller outside _WIN32");
#endif

    // The production spelling: `RunMultiReactorWindows` binds through exactly this call.
    auto bound = FastCache::BindAndListen(core::net::defaultAddressResolver(), "127.0.0.1", 0, /*backlog*/ 4);
    if (!bound.has_value())
        SKIP("this platform would not bind a loopback listener: " + bound.error());

    SECTION("a parked accept is cancelled, not merely failed")
    {
        // **Proved by the ANSWER, never by a pause.** This slept 250 ms and closed, trusting the pause to have put
        // the acceptor inside the syscall; an acceptor the scheduler had not yet run read `BadHandle`, and the case
        // went red with nothing wrong. The answer is the proof the pause could only hope for: `Cancelled` is a close
        // that found the accept PARKED, `BadHandle` one that landed before it. So a `BadHandle` attempt proved
        // nothing and is repeated on a fresh listener with twice the pause, `ParkAttempts` times at most -- and every
        // attempt reading `BadHandle` is still RED at the bound, which is what a close that stopped cancelling a
        // parked accept would read too, so the repetition cannot hide that regression. Any other answer ends the
        // attempts and is judged at once. Nothing ever dials these ports, so an accept that is reached parks.
        core::platform::NativeHandle listening = bound->handle;
        std::string attempts;
        std::optional<core::net::NetError> decisive;
        for (auto const attempt: std::views::iota(0, ParkAttempts))
        {
            if (attempt > 0)
            {
                auto again = FastCache::BindAndListen(core::net::defaultAddressResolver(), "127.0.0.1", 0, /*backlog*/ 4);
                REQUIRE(again.has_value());
                listening = again->handle;
            }
            auto const answer = CloseUnderParkedAccept(listening, FirstParkPause * (1 << attempt));
            if (!answer.has_value())
                FAIL("AcceptRaw was still parked 10s after its listening socket was closed, so the mechanism "
                     "RunMultiReactorWindows' stopAll depends on does not hold on this platform (#1238)");
            auto const& error = Unwrap(answer);
            attempts += std::format("{}{}", attempts.empty() ? "" : "; ", error.toString());
            if (error.code != core::net::NetErrorCode::BadHandle)
            {
                decisive = error;
                break;
            }
        }

        INFO("AcceptRaw returned, attempt by attempt: " << attempts);
        REQUIRE(decisive.has_value()); // every attempt's close landed before the accept parked
        CHECK(Unwrap(decisive).code == core::net::NetErrorCode::Cancelled);
    }

    SECTION("an accept CALLED after the close reports the other code")
    {
        core::platform::NativeHandle const listening = bound->handle;
        FastCache::CloseNativeSocket(listening);

        auto const accepted = FastCache::AcceptRaw(listening);
        REQUIRE_FALSE(accepted.has_value());
        INFO("AcceptRaw returned " << accepted.error().toString());
        CHECK(accepted.error().code == core::net::NetErrorCode::BadHandle);
    }
}

TEST_CASE("A signal that interrupts an armed accept's poll is a poll tick, not a closed listener",
          "[net][socket][listener][signal]")
{
    // Both binaries install a SIGHUP handler that nothing blocks on other threads, and a caught
    // signal interrupts a parked `poll()` with EINTR whatever SA_RESTART says. This row used to
    // answer `Cancelled` -- a closed listener, to every accept loop -- so one SIGHUP landing on the
    // admin thread ended its loop while the process lived. On POSIX a close never wakes the poll,
    // so EINTR can only be a signal, and it must read as the tick the poll answers anyway.
#if defined(_WIN32)
    SKIP("no POSIX signal reaches a Windows thread, and WSAEINTR there is a closed socket -- the close case pins it");
#else
    FastCache::Testing::ScopedSignalHandler const handler { SIGUSR2 };
    REQUIRE(handler.Installed());
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("this platform would not bind a loopback listener");
    // Far longer than the wait below, so the poll's own timeout cannot be what answers.
    listener->SetTimeouts(60s, 1000ms);

    std::promise<core::net::AcceptResult> outcome;
    auto answered = outcome.get_future();
    std::thread acceptor { [&listener, &outcome] { outcome.set_value(core::async::syncRun(AcceptOne(listener.get()))); } };
    auto const native = acceptor.native_handle();
    // Signalled on every poll until the accept answers: one landing before the thread is inside
    // `poll()` is handled and lost, and a later one lands inside it.
    auto const reached = FastCache::Testing::WaitUntil(
        "the parked accept to answer a signal",
        [&answered] { return answered.wait_for(0s) == std::future_status::ready; },
        [] { return std::string { "signalling the acceptor thread on every poll" }; },
        FastCache::Testing::WaitOptions { .step = [&handler, native] { handler.Interrupt(native); },
                                          .context = {},
                                          .bound = FastCache::Testing::WaitHangGuard,
                                          .rest = FastCache::Testing::WaitRest });
    // Joined before any assertion: the poll ends on its own within its timeout, so this is bounded.
    acceptor.join();
    REQUIRE(reached);

    auto const accepted = answered.get();
    REQUIRE_FALSE(accepted.has_value());
    INFO("the interrupted accept answered " << accepted.error().toString());
    // The signal answered, not the poll's timeout: that one carries no errno.
    CHECK(accepted.error().systemCode == EINTR);
    CHECK(core::net::acceptDispositionOf(accepted.error().code) == core::net::AcceptDisposition::PollTick);
#endif
}

namespace
{

/// One OS error an accept can answer, and what an accept loop must do about it -- written here
/// independently of `NativeListen.cpp`'s rows, so the two tables disagreeing is a failure.
struct ExpectedAcceptError
{
    int osError;                              ///< The platform's error number.
    std::string_view name;                    ///< Its spelling, for the failure message.
    core::net::AcceptDisposition disposition; ///< What the loop must do.
};

using enum core::net::AcceptDisposition;

#if defined(_WIN32)
constexpr auto ExpectedAcceptErrors = std::array {
    ExpectedAcceptError { .osError = WSAETIMEDOUT, .name = "WSAETIMEDOUT", .disposition = PollTick },
    ExpectedAcceptError { .osError = WSAEWOULDBLOCK, .name = "WSAEWOULDBLOCK", .disposition = PollTick },
    ExpectedAcceptError { .osError = WSAEINTR, .name = "WSAEINTR", .disposition = Closed },
    ExpectedAcceptError { .osError = WSAENOTSOCK, .name = "WSAENOTSOCK", .disposition = Dead },
    ExpectedAcceptError { .osError = WSAECONNRESET, .name = "WSAECONNRESET", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = WSAECONNABORTED, .name = "WSAECONNABORTED", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = WSAEMFILE, .name = "WSAEMFILE", .disposition = Exhausted },
    ExpectedAcceptError { .osError = WSAENOBUFS, .name = "WSAENOBUFS", .disposition = Exhausted },
    ExpectedAcceptError { .osError = WSA_NOT_ENOUGH_MEMORY, .name = "WSA_NOT_ENOUGH_MEMORY", .disposition = Exhausted },
};
#else
constexpr auto ExpectedAcceptErrors = std::array {
    ExpectedAcceptError { .osError = EAGAIN, .name = "EAGAIN", .disposition = PollTick },
    ExpectedAcceptError { .osError = EWOULDBLOCK, .name = "EWOULDBLOCK", .disposition = PollTick },
    ExpectedAcceptError { .osError = EINTR, .name = "EINTR", .disposition = PollTick },
    ExpectedAcceptError { .osError = EBADF, .name = "EBADF", .disposition = Dead },
    ExpectedAcceptError { .osError = ENOTSOCK, .name = "ENOTSOCK", .disposition = Dead },
    ExpectedAcceptError { .osError = ECONNABORTED, .name = "ECONNABORTED", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = EPERM, .name = "EPERM", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = EPROTO, .name = "EPROTO", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = ENOPROTOOPT, .name = "ENOPROTOOPT", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = EOPNOTSUPP, .name = "EOPNOTSUPP", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = ENETDOWN, .name = "ENETDOWN", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = ENETUNREACH, .name = "ENETUNREACH", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = EHOSTDOWN, .name = "EHOSTDOWN", .disposition = PeerFailed },
    ExpectedAcceptError { .osError = EHOSTUNREACH, .name = "EHOSTUNREACH", .disposition = PeerFailed },
    #if defined(ENONET)
    ExpectedAcceptError { .osError = ENONET, .name = "ENONET", .disposition = PeerFailed },
    #endif
    ExpectedAcceptError { .osError = EMFILE, .name = "EMFILE", .disposition = Exhausted },
    ExpectedAcceptError { .osError = ENFILE, .name = "ENFILE", .disposition = Exhausted },
    ExpectedAcceptError { .osError = ENOBUFS, .name = "ENOBUFS", .disposition = Exhausted },
    ExpectedAcceptError { .osError = ENOMEM, .name = "ENOMEM", .disposition = Exhausted },
};
#endif

} // namespace

TEST_CASE("Every OS error an accept can answer reaches the accept loop as the disposition it deserves",
          "[net][listener][accept-policy]")
{
    // `SocketErrorCode` is a second classifier in front of core-cpp's `AcceptErrorTable`, and a number
    // it has no row for is `SystemError` -- unclassified, a backoff of up to a second and, if it
    // persists, a surface reported degraded. It had no per-connection
    // rows at all, so a flood of resets on the Windows acceptor threads, or of ECONNABORTED and
    // firewall EPERM on a POSIX BlockingListener, read as a process out of descriptors. This binds
    // the two tables together: each OS error, through both, must land where an accept loop steps
    // past a failed connection, retries a tick, backs off, or stops.
    //
    // ONE assertion over every row, never one per row: rows sharing a code fail together when that
    // code's disposition moves, four of them share one, and ctest scores a Catch2 exit of exactly 4
    // as Skipped (#1152) -- so a per-row CHECK read "100% tests passed" for the very regression this
    // case exists for (`ConnReset -> Exhausted`, measured). At most two assertions fail here.
    std::vector<std::string> mismatches;
    for (auto const& expected: ExpectedAcceptErrors)
    {
        auto const code = FastCache::SocketErrorCode(expected.osError);
        auto const disposition = core::net::acceptDispositionOf(code);
        if (disposition != expected.disposition)
            mismatches.push_back(std::format("{} ({}) answers code {} -> disposition {}, expected {}",
                                             expected.name,
                                             expected.osError,
                                             static_cast<int>(code),
                                             static_cast<int>(disposition),
                                             static_cast<int>(expected.disposition)));
    }
    std::string listing;
    for (auto const& mismatch: mismatches)
        listing += std::format("\n  {}", mismatch);
    INFO("rows that land on the wrong disposition:" << listing);
    CHECK(mismatches.empty());
    // And a number no row names is unclassified, never an end: a code nobody thought of must not end
    // a loop by default.
    CHECK(FastCache::SocketErrorCode(-1) == core::net::NetErrorCode::SystemError);
}

TEST_CASE("Adopting a descriptor that is not a stream socket is refused by name", "[net][listener]")
{
    // accept(2) answers EOPNOTSUPP forever on a listener that is not SOCK_STREAM, and ALSO hands it
    // back as a network error pending on one new TCP connection -- so the accept table must step
    // past it, and a non-stream listener has to be refused where that fact is known: at adoption,
    // before any loop runs over it.
    core::platform::ensureWinsockInitialized();

    // The control: a real listening stream socket is adopted and serves.
    auto bound = BindLoopback(0);
    REQUIRE(bound.has_value());
    auto const stream = FastCache::BlockingListener::Adopt(bound->handle);
    CHECK(stream->IsBound());
    CHECK(stream->BindError().empty());

#if defined(_WIN32)
    auto* const datagram = reinterpret_cast<core::platform::NativeHandle>(::socket(AF_INET, SOCK_DGRAM, 0));
#else
    core::platform::NativeHandle const datagram = ::socket(AF_INET, SOCK_DGRAM, 0);
#endif
    REQUIRE(datagram != core::platform::InvalidHandle);
    // Owned by `Adopt` from here, refused or not, so the case never closes it.
    auto const refused = FastCache::BlockingListener::Adopt(datagram);
    CHECK_FALSE(refused->IsBound());
    INFO("the refusal was: " << refused->BindError());
    CHECK(refused->BindError().contains(std::format("socket type {}", static_cast<int>(SOCK_DGRAM))));
}

TEST_CASE("A supervisor's descriptor that is not a stream socket is refused by name", "[net][listener]")
{
    // Socket activation reaches the node's reactor listener through `AdoptInheritedListener`, and
    // core-cpp's `adoptListener` does not ask the type: a unit handing over a datagram socket would
    // start an accept loop whose every accept fails, forever. Refused before core-cpp takes it.
    core::platform::SteadyClock clock;
    core::net::PlatformLoop loop { clock };
#if defined(_WIN32)
    // Windows has no socket activation, so every number is refused before anything touches it.
    auto const refused = FastCache::AdoptInheritedListener(loop, 3);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("not available on this platform"));
#else
    auto const datagram = ::socket(AF_INET, SOCK_DGRAM, 0);
    REQUIRE(datagram >= 0);
    // Owned by the call from here, refused or not.
    auto const refused = FastCache::AdoptInheritedListener(loop, datagram);
    REQUIRE_FALSE(refused.has_value());
    INFO("the refusal was: " << refused.error());
    CHECK(refused.error().contains(std::format("socket type {}", static_cast<int>(SOCK_DGRAM))));
    // And closed, so a refusal leaks nothing.
    CHECK(::fcntl(datagram, F_GETFD) == -1);
#endif
}

namespace
{

/// A TCP socket bound to a loopback port and never listened on.
/// @return Its handle, or `InvalidHandle` when it could not be made; the caller owns it.
[[nodiscard]] core::platform::NativeHandle BoundUnlistenedStreamSocket()
{
    core::platform::ensureWinsockInitialized();
#if defined(_WIN32)
    using SockLen = int;
    auto const raw = ::socket(AF_INET, SOCK_STREAM, 0);
    auto* const handle = reinterpret_cast<core::platform::NativeHandle>(raw);
#else
    using SockLen = socklen_t;
    auto const raw = ::socket(AF_INET, SOCK_STREAM, 0);
    core::platform::NativeHandle const handle = raw;
#endif
    if (handle == core::platform::InvalidHandle)
        return core::platform::InvalidHandle;
    sockaddr_in loopback {};
    loopback.sin_family = AF_INET;
    loopback.sin_port = 0;
    loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(raw, reinterpret_cast<sockaddr const*>(&loopback), static_cast<SockLen>(sizeof(loopback))) != 0)
    {
        FastCache::CloseNativeSocket(handle);
        return core::platform::InvalidHandle;
    }
    return handle;
}

} // namespace

TEST_CASE("Adopting a stream socket nobody listens on is refused by name", "[net][listener]")
{
    // accept(2) answers EINVAL forever on a stream socket that was bound but never listened on, and
    // an accept loop reads that unclassified code as exhaustion -- a backoff and a warning, for as
    // long as the process runs, over a port that serves nothing. Refused at adoption instead, by
    // both doors: the blocking listener's and socket activation's.
    core::platform::NativeHandle const unlistened = BoundUnlistenedStreamSocket();
    REQUIRE(unlistened != core::platform::InvalidHandle);
    // Owned by `Adopt` from here, refused or not.
    auto const refused = FastCache::BlockingListener::Adopt(unlistened);
    CHECK_FALSE(refused->IsBound());
    INFO("the blocking listener's refusal was: " << refused->BindError());
    CHECK(refused->BindError().contains("SO_ACCEPTCONN"));

#if !defined(_WIN32)
    // Socket activation's door. Windows refuses every descriptor there before asking anything,
    // which the supervisor case asserts.
    core::platform::SteadyClock clock;
    core::net::PlatformLoop loop { clock };
    auto const inherited = BoundUnlistenedStreamSocket();
    REQUIRE(inherited != core::platform::InvalidHandle);
    auto const adopted = FastCache::AdoptInheritedListener(loop, inherited);
    REQUIRE_FALSE(adopted.has_value());
    INFO("socket activation's refusal was: " << adopted.error());
    CHECK(adopted.error().contains("SO_ACCEPTCONN"));
#endif
}
