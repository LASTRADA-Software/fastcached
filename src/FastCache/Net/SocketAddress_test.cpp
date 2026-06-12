// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Net/SocketAddress.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#if defined(_WIN32)
    #include <winsock2.h>

    #include <ws2tcpip.h>
#else
    #include <sys/socket.h>

    #include <unistd.h>

    #include <arpa/inet.h>
    #include <netinet/in.h>
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
