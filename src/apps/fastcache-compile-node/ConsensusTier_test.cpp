// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

TEST_CASE("A peer is an identity and an address, in one token", "[node][consensus]")
{
    // Both halves together because they are one fact. A member id with no address is
    // a node the cluster counts towards quorum and cannot reach -- the residual
    // `RaftMembership` recorded, and the reason a member carries its endpoint at all.
    auto const peer = ParsePeerSpec("n1=10.0.0.1:6680");
    REQUIRE(peer.has_value());
    CHECK(Unwrap(peer).id == "n1");
    CHECK(Unwrap(peer).endpoint == "10.0.0.1:6680");
}

TEST_CASE("A peer specification splits at the first separator", "[node][consensus]")
{
    // The endpoint may contain an `=` and the identity may not. Splitting at the LAST
    // one instead would read `n1=host=1:6675` as an id of `n1=host` -- an id no
    // operator wrote, which would then silently never match a vote and leave the
    // cluster one member short of a quorum it thinks it has.
    auto const odd = ParsePeerSpec("n1=weird=host:6680");
    REQUIRE(odd.has_value());
    CHECK(Unwrap(odd).id == "n1");
    CHECK(Unwrap(odd).endpoint == "weird=host:6680");
}

TEST_CASE("A peer with no dialable address is refused", "[node][consensus]")
{
    // Refused at startup, where an operator is watching. A member recorded with an
    // address nobody can dial registers, is counted towards every quorum, and is
    // never reached -- the fleet is then one node short of forming one and nothing
    // says why.
    CHECK_FALSE(ParsePeerSpec("n1").has_value());
    CHECK_FALSE(ParsePeerSpec("n1=").has_value());
    CHECK_FALSE(ParsePeerSpec("=10.0.0.1:6680").has_value());
    CHECK_FALSE(ParsePeerSpec("n1=10.0.0.1").has_value());
    CHECK_FALSE(ParsePeerSpec("").has_value());
}

TEST_CASE("An IPv6 peer keeps its address rather than its last colon group", "[node][consensus]")
{
    // Split through `Core/HostPort`, which is the whole reason that header exists: a
    // naive `rfind(':')` takes `[::1]:6680` apart at the wrong colon and yields a
    // host of `[::` and a port of `1]`.
    auto const peer = ParsePeerSpec("n1=[2001:db8::1]:6680");
    REQUIRE(peer.has_value());
    CHECK(Unwrap(peer).id == "n1");
    CHECK(Unwrap(peer).endpoint == "[2001:db8::1]:6680");
}
