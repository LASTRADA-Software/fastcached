// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "Responders.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
[[nodiscard]] std::vector<std::byte> Bytes(std::string_view text)
{
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (auto const ch: text)
        out.push_back(static_cast<std::byte>(ch));
    return out;
}

/// A node cache with nothing behind it, which is the single-machine shape.
struct Fixture
{
    // Field order is the analyzer's rather than the reading order; see
    // `LocalCache_test` for why a test fixture's padding is worth caring about.
    InMemoryLruStorage local { 64 * 1024 };
    NoUpstream upstream;
    ManualClock clock;
    AtomicMetricsSink metrics;
    LocalCache cache { local, upstream, clock, metrics };
    CacheProxy proxy { cache };
};

[[nodiscard]] std::optional<Wire::Status> StatusOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    return header.has_value() ? std::optional { header->status } : std::nullopt;
}

[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> reply)
{
    return reply.subspan(Wire::ReplyHeaderSize);
}

[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<Wire::ErrorCode>(reply[Wire::ReplyHeaderSize]);
}
} // namespace

TEST_CASE("A node stores and serves an object over the cache wire", "[node][cacheproxy]")
{
    // What makes `fastcache-cc` able to talk to localhost and still get a cache. The
    // launcher used to dial the shared daemon directly, so every hit crossed the
    // network -- including for objects this machine compiled minutes ago.
    Fixture fix;

    auto const stored = SyncRun(fix.proxy.Answer(Wire::EncodeStore(Wire::StoreRequest {
        .key = "k1", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = Bytes("object-one") })));
    REQUIRE(StatusOf(stored) == Wire::Status::Ok);

    auto const fetched = SyncRun(fix.proxy.Answer(Wire::EncodeFetch("k1")));
    REQUIRE(StatusOf(fetched) == Wire::Status::Ok);
    auto const payload = PayloadOf(fetched);
    CHECK(std::vector<std::byte> { payload.begin(), payload.end() } == Bytes("object-one"));
}

TEST_CASE("A miss is Miss, never Error", "[node][cacheproxy]")
{
    // The two were one byte once, and a rejected client saw an endlessly cold cache
    // with no diagnostic -- the build merely got slower, forever. A miss carries a
    // zero-length payload rather than no payload, uniformly with every other reply.
    Fixture fix;

    auto const reply = SyncRun(fix.proxy.Answer(Wire::EncodeFetch("never-stored")));
    REQUIRE(StatusOf(reply) == Wire::Status::Miss);
    CHECK(PayloadOf(reply).empty());
}

TEST_CASE("A node's cache port refuses the other ports' verbs, as a reply", "[node][cacheproxy]")
{
    // A node serves three surfaces on three ports. Each refuses the others' verbs
    // with a typed reply rather than a close, so a client that reached the wrong one
    // learns which instead of seeing something indistinguishable from a dead host.
    Fixture fix;

    auto const lease = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    CHECK(ErrorOf(SyncRun(fix.proxy.Answer(lease))) == Wire::ErrorCode::DispatchNotPermitted);

    auto const compile = Wire::EncodeCompile(Wire::CompileRequest {
        .leaseToken = "t", .fingerprint = "gcc-14", .args = {}, .source = {}, .acceptedCodecs = {}, .sourceName = "t.cpp" });
    CHECK(ErrorOf(SyncRun(fix.proxy.Answer(compile))) == Wire::ErrorCode::DispatchNotPermitted);
}

TEST_CASE("A frame that is not this protocol is the one condition that closes", "[node][cacheproxy]")
{
    Fixture fix;

    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, std::byte { 0x11 }, Wire::CurrentVersion, 0x01, 0);
    CHECK(SyncRun(fix.proxy.Answer(frame)).empty());
}

TEST_CASE("An unknown opcode is stepped over, not fatal", "[node][cacheproxy]")
{
    Fixture fix;

    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, 0xEE, 0);
    CHECK(ErrorOf(SyncRun(fix.proxy.Answer(frame))) == Wire::ErrorCode::UnknownOpcode);
}

TEST_CASE("The node's cache answers this machine and refuses a stranger", "[node][cache][membership]")
{
    // The bind already answers most of this -- the cache surface is loopback by
    // default -- but a bind is not a policy. An operator who widens it to share the
    // tier with their peers would otherwise be sharing this machine's entire build
    // output with everybody who can route to the port.
    //
    // Deliberately stricter than `fastcached`'s own cache, which serves non-members
    // on purpose: that one is shared infrastructure somebody operates, this is a
    // developer's private tier, and the two are different things that happen to speak
    // one protocol.
    Fixture fixture;
    Distributed::ClusterMembership membership { { "10.0.0.1:7000" } };
    CacheResponder responder { fixture.proxy, membership };

    auto const fetch = Wire::EncodeFetch("some-key");

    // A stranger is refused as a *reply*, never by closing: a client that cannot tell
    // a policy refusal from a dead host retries forever and reports a flaky network.
    auto const refused = Wire::DecodeReplyHeader(SyncRun(responder.Answer(fetch, "10.9.9.9")));
    REQUIRE(refused.has_value());
    CHECK(Unwrap(refused).status == Wire::Status::Error);

    // This machine gets the cache's own answer -- a miss, since nothing is stored.
    auto const local = Wire::DecodeReplyHeader(SyncRun(responder.Answer(fetch, "127.0.0.1")));
    REQUIRE(local.has_value());
    CHECK(Unwrap(local).status == Wire::Status::Miss);

    // And so does a listed peer, which is what makes widening the bind usable at all.
    auto const peer = Wire::DecodeReplyHeader(SyncRun(responder.Answer(fetch, "10.0.0.1")));
    REQUIRE(peer.has_value());
    CHECK(Unwrap(peer).status == Wire::Status::Miss);
}
