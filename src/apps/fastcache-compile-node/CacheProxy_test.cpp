// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "Responders.hpp"

#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/CompileCache/PathCanon.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/HostPort.hpp>
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

TEST_CASE("A node canonicalizes a stored value's regions against the producer's roots", "[node][cacheproxy]")
{
    // #319. This tier used to store the value bytes verbatim and ignore the roots
    // the client sent, on the reasoning that canonicalization is "the shared cache's
    // job" and that "what this tier stores is what this machine will replay". Both
    // held while a node was a private tier in front of `fastcached`. #229 made a
    // node the shared cache, and "this machine" is not one layout -- every checkout
    // on it is a different one.
    //
    // What that cost is not a missed optimization. A `/showIncludes` region is
    // replayed to the build system as the object's dependency list, so a region
    // still naming the PRODUCING checkout hands this build dependencies on files it
    // will never edit -- which no later edit here can invalidate. It also feeds
    // `RecordManifest`, where every foreign path classifies as toolchain and is
    // dropped, leaving a manifest that revalidates on the TU alone and serves its
    // object however the headers move.
    Fixture fix;

    CompileValue value;
    value.objectBlob = Bytes("OBJECT");
    value.textRegions.push_back(
        { .grammar = PathCanon::Grammar::ShowIncludes, .bytes = "Note: including file: /producer/src/dep.hpp\n" });

    auto const stored =
        SyncRun(fix.proxy.Answer(Wire::EncodeStore(Wire::StoreRequest { .key = "k-canon",
                                                                        .prefetchGroup = {},
                                                                        .srcRoot = "/producer/src",
                                                                        .buildTree = "/producer/build",
                                                                        .value = EncodeCompileValue(value) })));
    REQUIRE(StatusOf(stored) == Wire::Status::Ok);

    auto const fetched = SyncRun(fix.proxy.Answer(Wire::EncodeFetch("k-canon")));
    REQUIRE(StatusOf(fetched) == Wire::Status::Ok);
    auto const decoded = DecodeCompileValue(PayloadOf(fetched));
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->textRegions.size() == 1);

    // The producer's root is gone and a token stands in its place, which is what
    // lets any other checkout localize it to its own.
    CHECK(decoded->textRegions.front().bytes == "Note: including file: <SRCROOT>/dep.hpp\n");
    CHECK_FALSE(decoded->textRegions.front().bytes.contains("/producer/"));

    // The object blob is never a region and is never rewritten: it is machine code,
    // and a byte run inside it that looks like a path is not one.
    CHECK(decoded->objectBlob == Bytes("OBJECT"));
}

TEST_CASE("A value the node cannot decode is stored verbatim rather than refused", "[node][cacheproxy]")
{
    // Canonicalization must not turn this tier into a validator. An opaque value is
    // not a malformed one -- this endpoint is a cache, and deciding what a value may
    // contain belongs to the client that wrote it and the daemon that speaks the
    // whole protocol.
    Fixture fix;

    auto const stored = SyncRun(fix.proxy.Answer(Wire::EncodeStore(Wire::StoreRequest {
        .key = "k-opaque", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = Bytes("not-a-value") })));
    REQUIRE(StatusOf(stored) == Wire::Status::Ok);

    auto const fetched = SyncRun(fix.proxy.Answer(Wire::EncodeFetch("k-opaque")));
    REQUIRE(StatusOf(fetched) == Wire::Status::Ok);
    auto const payload = PayloadOf(fetched);
    CHECK(std::vector<std::byte> { payload.begin(), payload.end() } == Bytes("not-a-value"));
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

TEST_CASE("A node refuses AUTH with the one code the launcher steps over", "[node][cacheproxy]")
{
    // A wire contract between two binaries that do not link each other. The only
    // thing they share is the enumerator this test and `Cc::CacheProtocol`'s
    // "unsupported AUTH is stepped over" case both name, which is what keeps them
    // from drifting apart.
    //
    // `Cc::CacheProtocol` tolerates exactly one refusal for a verb an endpoint does
    // not implement -- `UnknownOpcode` -- and on it proceeds unauthenticated, which
    // is right against a tier that has no credential to check. Answered with
    // anything else the launcher treats the exchange as a hard error, and then every
    // compile for the life of the process is a miss reported as `rejected`.
    //
    // So the code is not cosmetic and it is not "more accurate as
    // `DispatchNotPermitted`": with that code, every `FASTCACHE_TOKEN`-configured
    // launcher pointed at a node had a permanent 0% hit rate, presenting exactly as
    // a cache that is merely cold. This asserts the value, not the category.
    Fixture fix;

    auto const auth = Wire::EncodeAuth(Wire::AuthRequest { .username = "bob", .secret = "s3cret" });
    CHECK(ErrorOf(SyncRun(fix.proxy.Answer(auth))) == Wire::ErrorCode::UnknownOpcode);

    // And an empty username, which is how a bare `FASTCACHE_TOKEN` with no user
    // reaches the wire, takes the same answer.
    auto const tokenOnly = Wire::EncodeAuth(Wire::AuthRequest { .username = "", .secret = "s3cret" });
    CHECK(ErrorOf(SyncRun(fix.proxy.Answer(tokenOnly))) == Wire::ErrorCode::UnknownOpcode);
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

TEST_CASE("REPRODUCTION (#287): a fleet peer reads another machine's cache tier",
          "[node][cache][membership][cache-locality]")
{
    // THE HOLE. `CacheResponder` gates on `Membership::Member`, and a Member is any
    // machine the operator listed -- so a peer on ANOTHER host reads this machine's
    // entire build output. The docs promise exactly that today ("its own machine and
    // its cluster"), which is why #287 is a deliberate tightening rather than a bug
    // fix, and why it ships as a breaking change.
    //
    // The peer here is ADMITTED, not unknown, and that distinction is the whole point
    // of the fixture: a reproduction whose caller is refused for some other reason
    // would pass under the bug and prove nothing about locality.
    Fixture fixture;
    Distributed::ClusterMembership membership { { "10.0.0.1:7000" } };
    CacheResponder responder { fixture.proxy, membership };

    // Stated first, so a later change to the membership vocabulary cannot turn this
    // case green by quietly reclassifying the peer.
    REQUIRE(membership.Classify("10.0.0.1") == Distributed::Membership::Member);

    // And it is not this machine, which is the property the fix will key on.
    REQUIRE_FALSE(IsLoopbackHost("10.0.0.1"));

    auto const served = Wire::DecodeReplyHeader(SyncRun(responder.Answer(Wire::EncodeFetch("some-key"), "10.0.0.1")));
    REQUIRE(served.has_value());

    // `Miss` rather than `Error`: the request reached the tier and was answered on
    // its merits. A stranger gets `Error`/`NotAMember` and never gets this far, so
    // this status IS the admission -- an empty cache is what makes it a miss rather
    // than a hit.
    //
    // When #287 lands this becomes `Status::Error` carrying `NotAMember`, and the
    // existing case above -- "and so does a listed peer, which is what makes widening
    // the bind usable at all" -- inverts with it. That sentence is the promise being
    // withdrawn.
    CHECK(Unwrap(served).status == Wire::Status::Miss);
}
