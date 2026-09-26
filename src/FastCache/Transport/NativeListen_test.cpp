// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Transport/NativeListen.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <core/async/SyncRun.hpp>
#include <core/async/Task.hpp>
#include <core/net/IListener.hpp>
#include <core/net/NetError.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/SocketAddress.hpp>
#include <core/net/Sockets.hpp>
#include <core/platform/Types.hpp>
#include <core/platform/WinsockInit.hpp>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
#endif

using namespace std::chrono_literals;

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

TEST_CASE("BoundPortOf reports 0 for a handle that is not bound", "[net][listener]")
{
    CHECK(FastCache::BoundPortOf(core::platform::InvalidHandle) == 0);
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
    REQUIRE(raw != INVALID_SOCKET);
    auto const native = reinterpret_cast<core::platform::NativeHandle>(raw);

    // The default is asserted too: a Windows socket IS inheritable unless something says
    // otherwise.
    DWORD before = 0;
    auto const handle = reinterpret_cast<HANDLE>(raw);
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
        auto const listening = bound->handle;

        // Nothing ever dials this port, so the accept genuinely parks.
        std::promise<core::net::NetError> outcome;
        auto answered = outcome.get_future();
        std::thread acceptor { [listening, &outcome] {
            auto accepted = FastCache::AcceptRaw(listening);
            if (accepted.has_value())
            {
                FastCache::CloseNativeSocket(accepted->handle);
                outcome.set_value(core::net::makeNetError(core::net::NetErrorCode::Ok, 0, "accept SUCCEEDED"));
                return;
            }
            outcome.set_value(std::move(accepted).error());
        } };

        // Long enough that the acceptor is inside the syscall. If it were not, the close would
        // land first and the code below would read `BadHandle` -- which is why the assertion is on
        // WHICH error: a too-short pause fails this case instead of passing it quietly.
        std::this_thread::sleep_for(250ms);
        FastCache::CloseNativeSocket(listening);

        if (answered.wait_for(10s) != std::future_status::ready)
        {
            // Detach rather than join: the thread is in a syscall nothing remaining can end.
            acceptor.detach();
            FAIL("AcceptRaw was still parked 10s after its listening socket was closed, so the mechanism "
                 "RunMultiReactorWindows' stopAll depends on does not hold on this platform (#1238)");
        }
        acceptor.join();

        auto const err = answered.get();
        INFO("AcceptRaw returned " << err.toString());
        CHECK(err.code == core::net::NetErrorCode::Cancelled);
    }

    SECTION("an accept CALLED after the close reports the other code")
    {
        auto const listening = bound->handle;
        FastCache::CloseNativeSocket(listening);

        auto const accepted = FastCache::AcceptRaw(listening);
        REQUIRE_FALSE(accepted.has_value());
        INFO("AcceptRaw returned " << accepted.error().toString());
        CHECK(accepted.error().code == core::net::NetErrorCode::BadHandle);
    }
}
