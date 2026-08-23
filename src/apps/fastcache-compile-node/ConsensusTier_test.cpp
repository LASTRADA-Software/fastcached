// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"

#include <FastCache/Core/HostPort.hpp>

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
    CHECK(Unwrap(peer).raftEndpoint == "10.0.0.1:6680");
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
    CHECK(Unwrap(odd).raftEndpoint == "weird=host:6680");
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
    CHECK(Unwrap(peer).raftEndpoint == "[2001:db8::1]:6680");
}

TEST_CASE("A leader advertises the port a client speaks to, at an address it can reach", "[node][consensus]")
{
    // Neither half can supply the other, which is the whole reason this is a
    // function. `--listen-scheduler` binds the WILDCARD for a bare port -- peers are
    // on other machines by definition -- so what the surface bound names no address a
    // client can dial. The consensus endpoint is dialable by construction, every peer
    // opening a socket to it, and names the wrong port.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "0.0.0.0:7000") == "10.0.0.1:7000");

    // A scheduler bound to one interface keeps its port and nothing else: the host is
    // the one peers have proved they can reach.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "127.0.0.1:7100") == "10.0.0.1:7100");
}

TEST_CASE("A node with no scheduler surface advertises nothing", "[node][consensus]")
{
    // A legitimate shape rather than a misconfiguration: a member that contributes
    // CPU and consensus without handing out anybody's work. Recording an endpoint for
    // it would redirect clients at a port nothing is listening on, which is worse
    // than redirecting them nowhere -- they would wait for a connect that cannot
    // succeed instead of compiling locally at once.
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "").empty());
    CHECK(AdvertisedSchedulerEndpoint("", "0.0.0.0:7000").empty());
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1", "0.0.0.0:7000").empty());
    CHECK(AdvertisedSchedulerEndpoint("10.0.0.1:6680", "7000").empty());
}

TEST_CASE("An IPv6 advertisement is bracketed, so it splits back the way it went in", "[node][consensus]")
{
    // `SplitHostPort` hands back a v6 host WITHOUT its brackets, and every consumer
    // of this string splits it again -- so it has to go back the way it came or the
    // next split takes the wrong colon. That is the defect `Core/HostPort` exists to
    // hold in one place, and this is one of the places.
    auto const advertised = AdvertisedSchedulerEndpoint("[2001:db8::1]:6680", "[::]:7000");
    CHECK(advertised == "[2001:db8::1]:7000");

    auto const split = SplitHostPort(advertised);
    REQUIRE(split.has_value());
    CHECK(Unwrap(split).first == "2001:db8::1");
    CHECK(Unwrap(split).second == "7000");
}
