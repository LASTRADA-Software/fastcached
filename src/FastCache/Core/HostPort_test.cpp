// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using FastCache::Testing::Unwrap;

TEST_CASE("An IPv6 literal is split at its bracket, not at its last colon", "[core][hostport]")
{
    // The whole reason this is shared rather than an `rfind(':')` at each caller.
    // Unbracketed, `::1:6674` splits into host `::1` and port `6674` by luck and
    // `fe80::1:6674` into host `fe80::1` and port `6674` by the same luck — but
    // `::1` alone splits into host `:` and port `1`, which resolves to nothing and
    // reports a connection failure rather than a malformed address.
    auto const bracketed = SplitHostPort("[::1]:6674");
    REQUIRE(bracketed.has_value());
    CHECK(Unwrap(bracketed).first == "::1");
    CHECK(Unwrap(bracketed).second == "6674");

    auto const full = SplitHostPort("[fe80::1%eth0]:9");
    REQUIRE(full.has_value());
    CHECK(Unwrap(full).first == "fe80::1%eth0");

    // A bracket with no port after it is malformed, not a host.
    CHECK_FALSE(SplitHostPort("[::1]").has_value());
    CHECK_FALSE(SplitHostPort("[::1]x6674").has_value());
}

TEST_CASE("An IPv4 endpoint splits at its only colon", "[core][hostport]")
{
    auto const split = SplitHostPort("127.0.0.1:6674");
    REQUIRE(split.has_value());
    CHECK(Unwrap(split).first == "127.0.0.1");
    CHECK(Unwrap(split).second == "6674");

    // No colon, or nothing after it: no port, so no endpoint.
    CHECK_FALSE(SplitHostPort("127.0.0.1").has_value());
    CHECK_FALSE(SplitHostPort("127.0.0.1:").has_value());
}

TEST_CASE("A port must be the whole text and must be usable", "[core][hostport]")
{
    CHECK(ParseTcpPort("6674") == std::optional { std::uint16_t { 6674 } });
    CHECK(ParseTcpPort("65535") == std::optional { std::uint16_t { 65535 } });

    // A trailing remainder is refused rather than ignored. Stopping at it would
    // bind a listener somewhere other than where the operator wrote, silently.
    CHECK_FALSE(ParseTcpPort("6674x").has_value());
    CHECK_FALSE(ParseTcpPort(" 6674").has_value());
    CHECK_FALSE(ParseTcpPort("").has_value());

    // Out of range, and the one in-range value that is not an endpoint: 0 means
    // "any free port" to the kernel, which nobody can be told in advance.
    CHECK_FALSE(ParseTcpPort("65536").has_value());
    CHECK_FALSE(ParseTcpPort("0").has_value());
    CHECK_FALSE(ParseTcpPort("-1").has_value());
}

TEST_CASE("A bare port binds the default host rather than the wildcard", "[core][hostport]")
{
    // What makes `--admin-listen 9100` safe to type: a scrape surface reachable
    // from the network is an operator's decision, and defaulting to 0.0.0.0 would
    // make it an accident.
    auto const bare = ParseEndpoint("9100", "127.0.0.1");
    REQUIRE(bare.has_value());
    CHECK(Unwrap(bare).first == "127.0.0.1");
    CHECK(Unwrap(bare).second == 9100);

    // An explicit host still wins, including the wildcard when it is asked for.
    auto const explicitHost = ParseEndpoint("0.0.0.0:9100", "127.0.0.1");
    REQUIRE(explicitHost.has_value());
    CHECK(Unwrap(explicitHost).first == "0.0.0.0");
    CHECK(Unwrap(explicitHost).second == 9100);

    auto const v6 = ParseEndpoint("[::]:9100", "127.0.0.1");
    REQUIRE(v6.has_value());
    CHECK(Unwrap(v6).first == "::");

    // A host with an unusable port is an error, not a fall-through to the bare
    // form — otherwise `example.com:0` would quietly become port `example.com`.
    CHECK_FALSE(ParseEndpoint("127.0.0.1:0", "127.0.0.1").has_value());
    CHECK_FALSE(ParseEndpoint("127.0.0.1:abc", "127.0.0.1").has_value());
    CHECK_FALSE(ParseEndpoint("", "127.0.0.1").has_value());
}

TEST_CASE("An IPv4-mapped host folds to the address it maps", "[core][hostport]")
{
    // One machine, two spellings, and which one a caller sees depends on how the
    // listener was BOUND rather than on anything about the peer.
    CHECK(UnmappedHost("::ffff:10.0.0.1") == "10.0.0.1");
    CHECK(UnmappedHost("::ffff:127.0.0.1") == "127.0.0.1");

    // Anything that is not mapped comes back unchanged, `::1` included — it is a
    // genuine IPv6 address rather than a wrapper around a v4 one.
    CHECK(UnmappedHost("10.0.0.1") == "10.0.0.1");
    CHECK(UnmappedHost("::1") == "::1");
    CHECK(UnmappedHost("2001:db8::1") == "2001:db8::1");
    CHECK(UnmappedHost("worker-01.internal") == "worker-01.internal");
    CHECK(UnmappedHost("").empty());

    // The prefix is matched whole. `::ffff` without its colon is not the mapped
    // form, and stripping a partial one would invent an address.
    CHECK(UnmappedHost("::ffff") == "::ffff");
}

TEST_CASE("Loopback still answers for every spelling a kernel produces", "[core][hostport]")
{
    // `IsLoopbackHost` is expressed through `UnmappedHost` now, so this pins that the
    // rewrite kept every answer. It is a security decision on two surfaces — the
    // membership oracle and the cache — and a regression here admits or refuses the
    // wrong machine silently.
    CHECK(IsLoopbackHost("127.0.0.1"));
    CHECK(IsLoopbackHost("127.0.0.53"));
    CHECK(IsLoopbackHost("::1"));
    CHECK(IsLoopbackHost("::ffff:127.0.0.1"));

    CHECK_FALSE(IsLoopbackHost("10.0.0.1"));
    CHECK_FALSE(IsLoopbackHost("::ffff:10.0.0.1"));
    CHECK_FALSE(IsLoopbackHost("128.0.0.1"));
    CHECK_FALSE(IsLoopbackHost(""));

    // Whatever a resolver says it is, which is not something a security decision may
    // depend on.
    CHECK_FALSE(IsLoopbackHost("localhost"));
}
