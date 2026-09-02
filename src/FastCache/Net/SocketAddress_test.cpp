// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Net/KeepAlive.hpp>
#include <FastCache/Net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
#endif

namespace
{

/// Close a raw socket handle returned by BindAndListen so tests do not leak
/// listeners across cases.
void CloseRaw(FastCache::Detail::NativeSocket socket) noexcept
{
#if defined(_WIN32)
    std::ignore = ::closesocket(static_cast<SOCKET>(socket));
#else
    std::ignore = ::close(static_cast<int>(socket));
#endif
}

/// Build an IPv4 ResolvedEndpoint for `ip`:`port`, mirroring what a resolver
/// would hand to BindAndListen. Lets tests script the candidate list
/// deterministically, with no dependency on real DNS.
FastCache::ResolvedEndpoint MakeV4Endpoint(char const* ip, std::uint16_t port)
{
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    ::inet_pton(AF_INET, ip, &sa.sin_addr);

    FastCache::ResolvedEndpoint endpoint;
    std::memcpy(endpoint.storage.data(), &sa, sizeof(sa));
    endpoint.length = sizeof(sa);
    endpoint.family = AF_INET;
    endpoint.protocol = IPPROTO_TCP;
    return endpoint;
}

/// Build an IPv6 ResolvedEndpoint for `ip`:`port` (e.g. the "::" wildcard),
/// mirroring what the system resolver hands to BindAndListen for an IPv6 bind.
FastCache::ResolvedEndpoint MakeV6Endpoint(char const* ip, std::uint16_t port)
{
    sockaddr_in6 sa {};
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(port);
    ::inet_pton(AF_INET6, ip, &sa.sin6_addr);

    FastCache::ResolvedEndpoint endpoint;
    std::memcpy(endpoint.storage.data(), &sa, sizeof(sa));
    endpoint.length = sizeof(sa);
    endpoint.family = AF_INET6;
    endpoint.protocol = IPPROTO_TCP;
    return endpoint;
}

/// Resolver fake that replays a fixed candidate list (or a fixed error),
/// ignoring host/port — the DI seam that makes BindAndListen testable without
/// touching the network.
class FakeAddressResolver final: public FastCache::IAddressResolver
{
  public:
    explicit FakeAddressResolver(std::vector<FastCache::ResolvedEndpoint> endpoints):
        _endpoints { std::move(endpoints) }
    {
    }

    explicit FakeAddressResolver(std::string error):
        _error { std::move(error) }
    {
    }

    std::expected<std::vector<FastCache::ResolvedEndpoint>, std::string> Resolve(std::string_view /*host*/,
                                                                                 std::uint16_t /*port*/) override
    {
        if (!_error.empty())
            return std::unexpected(_error);
        return _endpoints;
    }

  private:
    std::vector<FastCache::ResolvedEndpoint> _endpoints;
    std::string _error;
};

/// Bind 127.0.0.1:`port` through BindAndListen, the way a production listener
/// does.
/// @param port Port to ask for; 0 asks the kernel for an ephemeral one.
/// @param reusePort Passed through, so a case can hold a port shareably.
/// @return What BindAndListen answered.
[[nodiscard]] std::expected<FastCache::Detail::BoundListener, std::string> BindLoopback(
    std::uint16_t port, FastCache::ReusePort reusePort = FastCache::ReusePort::No)
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("127.0.0.1", port) } };
    return FastCache::Detail::BindAndListen(resolver, "127.0.0.1", port, /*backlog*/ 16, /*extraTypeFlags*/ 0, reusePort);
}

} // namespace

TEST_CASE("SystemAddressResolver resolves an IPv4 literal to a single AF_INET endpoint", "[net][resolver]")
{
    FastCache::SystemAddressResolver resolver;
    auto const resolved = resolver.Resolve("127.0.0.1", 11211);
    REQUIRE(resolved.has_value());
    REQUIRE(resolved->size() == 1);
    REQUIRE(resolved->front().family == AF_INET);
    REQUIRE(resolved->front().length >= sizeof(sockaddr_in));
}

TEST_CASE("SystemAddressResolver resolves an IPv6 literal to an AF_INET6 endpoint", "[net][resolver]")
{
    FastCache::SystemAddressResolver resolver;
    auto const resolved = resolver.Resolve("::1", 11211);
    REQUIRE(resolved.has_value());
    REQUIRE_FALSE(resolved->empty());
    REQUIRE(resolved->front().family == AF_INET6);
}

TEST_CASE("SystemAddressResolver resolves the localhost hostname", "[net][resolver]")
{
    FastCache::SystemAddressResolver resolver;
    auto const resolved = resolver.Resolve("localhost", 11211);
    REQUIRE(resolved.has_value());
    REQUIRE_FALSE(resolved->empty());
}

TEST_CASE("SystemAddressResolver resolves wildcard addresses", "[net][resolver]")
{
    FastCache::SystemAddressResolver resolver;

    SECTION("IPv4 wildcard")
    {
        auto const resolved = resolver.Resolve("0.0.0.0", 11211);
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->front().family == AF_INET);
    }
    SECTION("IPv6 wildcard")
    {
        auto const resolved = resolver.Resolve("::", 11211);
        REQUIRE(resolved.has_value());
        REQUIRE(resolved->front().family == AF_INET6);
    }
    SECTION("empty host is the passive wildcard")
    {
        auto const resolved = resolver.Resolve("", 11211);
        REQUIRE(resolved.has_value());
        REQUIRE_FALSE(resolved->empty());
    }
}

TEST_CASE("SystemAddressResolver rejects an unresolvable name", "[net][resolver]")
{
    FastCache::SystemAddressResolver resolver;
    // ".invalid" is a reserved TLD (RFC 6761) guaranteed never to resolve.
    auto const resolved = resolver.Resolve("fastcached.invalid", 11211);
    REQUIRE_FALSE(resolved.has_value());
    REQUIRE(resolved.error().contains("fastcached.invalid"));
}

TEST_CASE("BindAndListen binds the first resolved candidate", "[net][bind]")
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("127.0.0.1", 0) } };
    auto bound = FastCache::Detail::BindAndListen(resolver, "127.0.0.1", 0, /*backlog*/ 16, /*extraTypeFlags*/ 0);
    REQUIRE(bound.has_value());
    REQUIRE(bound->socket != FastCache::Detail::InvalidSocket);
    REQUIRE(bound->family == AF_INET);
    CloseRaw(bound->socket);
}

TEST_CASE("BindAndListen falls over to the next candidate when the first cannot bind", "[net][bind]")
{
    // 192.0.2.1 is TEST-NET-1 (RFC 5737): never assigned to a local interface,
    // so bind() fails with EADDRNOTAVAIL and the loop tries the next candidate.
    FakeAddressResolver resolver { std::vector {
        MakeV4Endpoint("192.0.2.1", 0),
        MakeV4Endpoint("127.0.0.1", 0),
    } };
    auto bound = FastCache::Detail::BindAndListen(resolver, "ignored", 0, /*backlog*/ 16, /*extraTypeFlags*/ 0);
    REQUIRE(bound.has_value());
    REQUIRE(bound->family == AF_INET);
    CloseRaw(bound->socket);
}

TEST_CASE("BindAndListen propagates a resolver error", "[net][bind]")
{
    FakeAddressResolver resolver { std::string { "cannot resolve 'banana': no usable address" } };
    auto const bound = FastCache::Detail::BindAndListen(resolver, "banana", 11211, /*backlog*/ 16, /*extraTypeFlags*/ 0);
    REQUIRE_FALSE(bound.has_value());
    REQUIRE(bound.error().contains("banana"));
}

TEST_CASE("BindAndListen reports failure when no candidate is bindable", "[net][bind]")
{
    FakeAddressResolver resolver { std::vector { MakeV4Endpoint("192.0.2.1", 0) } };
    auto const bound = FastCache::Detail::BindAndListen(resolver, "192.0.2.1", 0, /*backlog*/ 16, /*extraTypeFlags*/ 0);
    REQUIRE_FALSE(bound.has_value());
}

TEST_CASE("BindAndListen keeps its address to itself while it is listening", "[net][bind][exclusive]")
{
    // A listening address is not shareable, and saying so takes a different
    // option on each platform. POSIX SO_REUSEADDR only lets a bind step over a
    // TIME_WAIT left by a *dead* socket; Windows SO_REUSEADDR lets a second
    // socket bind an address a *live* one already holds -- so setting it there
    // let any process on the box take the port fastcached was already serving,
    // with which of the two answered a given connection left undefined
    // (issue #85). For a compile cache reached without a credential that is
    // object injection into everybody's build.
    //
    // Both sides go through BindAndListen on purpose: that is the production
    // shape -- one daemon already serving, a second asking for the same port --
    // and it is the option BindAndListen sets that has to refuse it.
    auto held = BindLoopback(0);
    REQUIRE(held.has_value());

    auto const port = FastCache::Detail::BoundPortOf(held->socket);
    REQUIRE(port != 0);

    auto const taken = BindLoopback(port);
    // CHECK rather than REQUIRE so the cleanup below still runs when this
    // regresses: a failure should be a failure, not also a leaked listener the
    // rest of the suite has to work around.
    CHECK_FALSE(taken.has_value());
    if (taken.has_value())
        CloseRaw(taken->socket);

    CloseRaw(held->socket);
}

#if defined(SO_REUSEPORT)
TEST_CASE("ReusePort::Yes still lets several listeners share one port", "[net][bind][reuseport]")
{
    // Exclusivity is the default, not the only setting. The POSIX multi-reactor
    // binds one listener per reactor on the same {address, port} and lets the
    // kernel load-balance across them (ReactorServerLoop's RunMultiReactorPosix),
    // so a change that made every bind exclusive would present as a daemon that
    // refuses to start with --threads greater than one. Windows has no
    // SO_REUSEPORT and reaches that shape another way -- one listener per bind,
    // handing raw sockets to N IOCP reactors -- so there is nothing to assert
    // there.
    auto held = BindLoopback(0, FastCache::ReusePort::Yes);
    REQUIRE(held.has_value());

    auto const port = FastCache::Detail::BoundPortOf(held->socket);
    REQUIRE(port != 0);

    auto shared = BindLoopback(port, FastCache::ReusePort::Yes);
    REQUIRE(shared.has_value());

    CloseRaw(shared->socket);
    CloseRaw(held->socket);
}
#endif

TEST_CASE("FormatPeerAddress renders the host without the port", "[net][peer]")
{
    SECTION("IPv4 dotted-quad")
    {
        REQUIRE(FastCache::FormatPeerAddress(MakeV4Endpoint("203.0.113.7", 54321)) == "203.0.113.7");
    }
    SECTION("IPv6 textual address")
    {
        REQUIRE(FastCache::FormatPeerAddress(MakeV6Endpoint("::1", 54321)) == "::1");
        REQUIRE(FastCache::FormatPeerAddress(MakeV6Endpoint("2001:db8::1", 0)) == "2001:db8::1");
    }
}

TEST_CASE("FormatPeerAddress returns empty for an unknown or empty endpoint", "[net][peer]")
{
    SECTION("default-constructed endpoint (length 0)")
    {
        REQUIRE(FastCache::FormatPeerAddress(FastCache::ResolvedEndpoint {}).empty());
    }
    SECTION("non-zero length but unsupported family")
    {
        FastCache::ResolvedEndpoint endpoint;
        endpoint.length = 8;
        endpoint.family = AF_UNSPEC;
        REQUIRE(FastCache::FormatPeerAddress(endpoint).empty());
    }
}

TEST_CASE("EndpointFromSockaddr round-trips through FormatPeerAddress", "[net][peer]")
{
    // Mirror what a listener does: a sockaddr filled by accept() is copied into
    // a ResolvedEndpoint, which formats back to the original host string.
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(40000);
    ::inet_pton(AF_INET, "198.51.100.23", &sa.sin_addr);

    auto const endpoint = FastCache::Detail::EndpointFromSockaddr(&sa, sizeof(sa));
    REQUIRE(endpoint.family == AF_INET);
    REQUIRE(endpoint.length == sizeof(sa));
    REQUIRE(FastCache::FormatPeerAddress(endpoint) == "198.51.100.23");
}

TEST_CASE("EndpointFromSockaddr rejects a zero or oversized length", "[net][peer]")
{
    sockaddr_in sa {};
    sa.sin_family = AF_INET;
    REQUIRE(FastCache::Detail::EndpointFromSockaddr(&sa, 0).length == 0);
    REQUIRE(FastCache::Detail::EndpointFromSockaddr(&sa, FastCache::ResolvedEndpoint::StorageSize + 1).length == 0);
}

TEST_CASE("BindAndListen forces dual-stack (IPV6_V6ONLY=0) on an IPv6 wildcard bind", "[net][bind][dual-stack]")
{
    FakeAddressResolver resolver { std::vector { MakeV6Endpoint("::", 0) } };
    auto bound = FastCache::Detail::BindAndListen(resolver, "::", 0, /*backlog*/ 16, /*extraTypeFlags*/ 0);
    if (!bound.has_value())
    {
        // No usable IPv6 stack in this environment — nothing to assert.
        SUCCEED("IPv6 unavailable; skipping dual-stack assertion");
        return;
    }
    REQUIRE(bound->family == AF_INET6);

    int v6only = 1;
#if defined(_WIN32)
    int len = sizeof(v6only);
    auto const rc =
        ::getsockopt(static_cast<SOCKET>(bound->socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&v6only), &len);
#else
    socklen_t len = sizeof(v6only);
    auto const rc = ::getsockopt(static_cast<int>(bound->socket), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, &len);
#endif
    REQUIRE(rc == 0);
    REQUIRE(v6only == 0); // dual-stack: the "::" socket also accepts IPv4 clients
    CloseRaw(bound->socket);
}

TEST_CASE("IsNumericHost recognises literals and rejects names", "[net][resolve]")
{
    // A table rather than a run of CHECKs: the point of the helper is that one
    // definition answers for every shape, and a table is what makes a new shape
    // a row. The expectation column carries the reason where it is not obvious.
    struct Row
    {
        std::string_view host;
        bool numeric;
        std::string_view why;
    };

    constexpr std::array<Row, 11> Rows { {
        { .host = "127.0.0.1",
          .numeric = true,
          .why = "the launcher's default, and the case that must never reach a thread" },
        { .host = "0.0.0.0",
          .numeric = true,
          .why = "a literal even though it is not dialable; the dial guard refuses it elsewhere" },
        { .host = "203.0.113.7", .numeric = true, .why = "ordinary IPv4" },
        { .host = "::1", .numeric = true, .why = "IPv6 loopback" },
        { .host = "::", .numeric = true, .why = "IPv6 wildcard" },
        { .host = "2001:db8::1", .numeric = true, .why = "ordinary IPv6" },
        { .host = "localhost", .numeric = false, .why = "a name, even though it almost always resolves to a literal" },
        { .host = "cache.example.com", .numeric = false, .why = "a name" },
        { .host = "", .numeric = false, .why = "no host at all" },
        { .host = "127.0.0.1:6674",
          .numeric = false,
          .why = "a host:port pair is not a host; splitting is the caller's job" },
        { .host = "[::1]",
          .numeric = false,
          .why = "brackets are endpoint grammar, and Connect is documented as taking them off" },
    } };

    for (auto const& row: Rows)
    {
        INFO(row.host << " -- " << row.why);
        CHECK(FastCache::Detail::IsNumericHost(row.host) == row.numeric);
    }
}

TEST_CASE("BoundPortOf reports the port the kernel chose, not the one asked for", "[net][listener]")
{
    // Binding port 0 is how every script-driven test here allocates a port, and
    // the value it needs back is the kernel's choice. Before this helper only
    // BlockingListener could answer, so a caller holding an IListener could not.
    auto listener = FastCache::BlockingListener::Bind("127.0.0.1", 0);
    REQUIRE(listener != nullptr);
    if (!listener->IsBound())
        SKIP("cannot bind loopback here");

    auto const port = listener->BoundPort();
    CHECK(port != 0);

    // And it is stable: the question is about the socket, not about the call.
    CHECK(listener->BoundPort() == port);
}

TEST_CASE("BoundPortOf reports 0 for a handle that is not bound", "[net][listener]")
{
    CHECK(FastCache::Detail::BoundPortOf(FastCache::Detail::InvalidSocket) == 0);
}

TEST_CASE("A scoped IPv6 literal is classified by the platform, and either answer works", "[net][resolve]")
{
    // Left OUT of the table above because the two platforms genuinely disagree:
    // glibc's `inet_pton` rejects the zone suffix and macOS's accepts it. Found the
    // way such things are -- green on Linux and Windows, red on macOS.
    //
    // Neither answer is wrong. A literal goes straight to `connect`; a name goes to
    // the resolver, which also understands zones. So what is asserted here is the
    // half that MUST hold -- the classification is stable -- rather than a value
    // that would make the suite fail for a reason about the host.
    constexpr std::string_view Scoped = "fe80::1%eth0";
    auto const first = FastCache::Detail::IsNumericHost(Scoped);
    CHECK(FastCache::Detail::IsNumericHost(Scoped) == first);

    // And the direction that would actually break IS pinned: a name must never be
    // reported as a literal, because it would then be handed to `connect` as an
    // address rather than looked up.
    CHECK_FALSE(FastCache::Detail::IsNumericHost("fe80-scoped.example.com"));
}

TEST_CASE("A socket this process owns is not handed to the children it spawns", "[net][listener]")
{
    // `fastcache-compile-node` accepts connections and spawns a compiler for every
    // job, so a socket a child inherits is a client connection that stays open for
    // as long as an unrelated compile runs. `ApplyHotSocketOptions` is where this is
    // armed because it is the one function EVERY socket this process owns passes
    // through -- accepted and dialled, on all four backends.

    // Winsock is per process and lazily started; every other case here reaches it
    // through BindAndListen, and a bare `::socket` before it returns INVALID_SOCKET.
    FastCache::Detail::EnsureNetworkInitialised();

    auto const native = static_cast<FastCache::Detail::NativeSocket>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(native != FastCache::Detail::InvalidSocket);

#if defined(_WIN32)
    // The default is asserted too, and it is the half that was got wrong: a Windows
    // socket IS inheritable unless something says otherwise, which is exactly the
    // opposite of what this code used to claim while doing nothing.
    DWORD before = 0;
    auto const handle = reinterpret_cast<HANDLE>(static_cast<SOCKET>(native));
    REQUIRE(::GetHandleInformation(handle, &before) != 0);
    CHECK((before & HANDLE_FLAG_INHERIT) != 0);

    FastCache::Detail::ApplyHotSocketOptions(native);

    DWORD after = 0;
    REQUIRE(::GetHandleInformation(handle, &after) != 0);
    CHECK((after & HANDLE_FLAG_INHERIT) == 0);
#else
    // Same on POSIX: a plain `::socket` is NOT close-on-exec, and `::accept` does
    // not inherit the flag from its listener either.
    auto const fd = static_cast<int>(native);
    REQUIRE(::fcntl(fd, F_GETFD) >= 0);
    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);

    FastCache::Detail::ApplyHotSocketOptions(native);

    CHECK((::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0);
#endif

    CloseRaw(native);
}

namespace
{

/// Read `SO_KEEPALIVE` back off a raw socket.
///
/// The flag rather than the intervals, because the flag is the half that is
/// portable to read: Windows sets its intervals through `SIO_KEEPALIVE_VALS` and
/// offers no matching get, so a case that insisted on reading them back could only
/// run on two of the three platforms -- and a case that does not run is not a case.
/// The intervals are asserted where they can be, below.
/// @param socket Handle to inspect.
/// @return True when the socket is probing.
[[nodiscard]] bool KeepAliveIsOn(FastCache::Detail::NativeSocket socket)
{
    int value = 0;
#if defined(_WIN32)
    auto length = static_cast<int>(sizeof(value));
    REQUIRE(::getsockopt(static_cast<SOCKET>(socket), SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<char*>(&value), &length)
            == 0);
#else
    auto length = static_cast<socklen_t>(sizeof(value));
    REQUIRE(::getsockopt(static_cast<int>(socket), SOL_SOCKET, SO_KEEPALIVE, &value, &length) == 0);
#endif
    return value != 0;
}

} // namespace

TEST_CASE("Keepalive is armed only where it is asked for", "[net][socket][keepalive]")
{
    // **The negative half is the point, and it is asserted first**
    // ([#247](https://github.com/LASTRADA-Software/fastcached/issues/247)). The
    // obvious home for keepalive is `ApplyHotSocketOptions`, which every socket this
    // process owns passes through -- and arming it there would silently change when
    // an idle memcached or Redis client connection is dropped and when a Raft peer
    // link is torn down, fleet-wide, for a change nobody asked for. Nothing about
    // the dispatch path would look wrong; the daemon's would.
    //
    // So the acceptance is "no other socket in the process changes behaviour", and
    // "assert this, do not assume it" -- the same shape as the close-on-exec case
    // above, where the BEFORE assertion is the half that caught the old bug.
    FastCache::Detail::EnsureNetworkInitialised();

    auto const native = static_cast<FastCache::Detail::NativeSocket>(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(native != FastCache::Detail::InvalidSocket);

    // A fresh socket does not probe. Stated, because the whole claim below is
    // relative to it: if the platform default were already on, "armed" would prove
    // nothing.
    CHECK_FALSE(KeepAliveIsOn(native));

    // And the hot options -- the function EVERY socket takes -- must leave it that
    // way. This is the assertion that fails if somebody later moves `ArmKeepAlive`
    // into `ApplyHotSocketOptions` for tidiness.
    FastCache::Detail::ApplyHotSocketOptions(native);
    CHECK_FALSE(KeepAliveIsOn(native));

    // Only the explicit call arms it.
    CHECK(FastCache::Detail::ArmKeepAlive(native, FastCache::KeepAliveSettings {}));
    CHECK(KeepAliveIsOn(native));

#if !defined(_WIN32)
    // Where the intervals can be read back, they are -- because the flag ALONE is
    // worth nothing: without them the socket inherits the system default, two hours
    // on Linux, which is longer than any deadline this protects while reading back
    // as armed. A case that checked only the flag would pass under exactly that bug.
    #if defined(__APPLE__)
    constexpr int IdleOption = TCP_KEEPALIVE;
    #else
    constexpr int IdleOption = TCP_KEEPIDLE;
    #endif
    constexpr FastCache::KeepAliveSettings Defaults {};

    auto const readBack = [native](int option) {
        int value = 0;
        auto length = static_cast<socklen_t>(sizeof(value));
        REQUIRE(::getsockopt(static_cast<int>(native), IPPROTO_TCP, option, &value, &length) == 0);
        return value;
    };

    CHECK(readBack(IdleOption) == std::chrono::duration_cast<std::chrono::seconds>(Defaults.idle).count());
    CHECK(readBack(TCP_KEEPINTVL) == std::chrono::duration_cast<std::chrono::seconds>(Defaults.interval).count());
    CHECK(readBack(TCP_KEEPCNT) == static_cast<int>(Defaults.count));

    // The bar the values exist to clear: a dead host noticed in well under a minute,
    // against a dispatch deadline that is minutes long by design since #223. Derived
    // from the settings rather than restated, so a future retune cannot satisfy the
    // literal and break the property.
    auto const probesStopAt = Defaults.idle + Defaults.count * Defaults.interval;
    CHECK(probesStopAt < std::chrono::seconds { 60 });
#else
    // Windows fixes the probe count at 10 and there is no getter for the intervals,
    // so the arithmetic is what can be checked here -- and it is worth checking,
    // because 10 probes is what makes this platform the slowest of the three and the
    // one the interval had to be chosen for.
    constexpr FastCache::KeepAliveSettings Defaults {};
    constexpr int WindowsFixedProbeCount = 10;
    auto const probesStopAt = Defaults.idle + WindowsFixedProbeCount * Defaults.interval;
    CHECK(probesStopAt < std::chrono::seconds { 60 });
#endif

    CloseRaw(native);
}
