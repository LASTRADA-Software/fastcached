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
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/LocalAddressesTestUtils.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
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
    CacheProxy proxy { cache, metrics };
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

TEST_CASE("A stored value from another generation is refused, not stored verbatim", "[node][cacheproxy]")
{
    // #483, and the one arm above where this tier gets no policy choice. A value it
    // cannot canonicalize still carries the PRODUCER's absolute paths, so storing it
    // would put them in the shared cache under a key every machine computes -- which
    // is #229/#319, reached by nothing worse than a rolling upgrade, because a fleet
    // is permanently mid-upgrade (#173).
    //
    // What separates this from the verbatim case above is the LAYOUT, not the leading
    // byte: this frame is one a launcher would have written, stamped with a
    // generation this build does not implement.
    Fixture fix;

    CompileValue produced;
    produced.objectBlob = { std::byte { 0x01 } };
    produced.textRegions.push_back(
        TextRegion { .grammar = PathCanon::Grammar::ShowIncludes, .bytes = "Note: including file: /src/inc/a.hpp\n" });
    auto foreign = EncodeCompileValue(produced);
    REQUIRE(static_cast<std::uint8_t>(foreign.front()) == CompileValueVersion);
    foreign.front() = std::byte { CompileValueVersion + 1 };

    auto const stored = SyncRun(fix.proxy.Answer(Wire::EncodeStore(Wire::StoreRequest {
        .key = "k-foreign", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = foreign })));

    // #544: its own code, not `MalformedValue`. This surface is where that mattered
    // most -- since #229 a node IS the shared cache, so it is the server a launcher
    // actually talks to, and `MalformedValue` told it the cache was damaged while the
    // fleet was merely mid-rollout. Same conflation `.agent/rules/storage.md` forbids
    // on disk between `Corrupt` and `UnsupportedFormatVersion`.
    //
    // The other half of the pair is the case immediately above: absence of the
    // negative is not the positive, so "a foreign generation is refused" needs
    // "opaque bytes are NOT" beside it, or a change that refused everything it could
    // not canonicalize would satisfy this one on its own. On this tier that companion
    // is a different KIND of answer rather than a different code, which is why it is
    // its own case and is not restated here.
    CHECK(ErrorOf(stored) == Wire::ErrorCode::ForeignValueGeneration);

    // Refused means nothing was written. A `Miss` rather than the bytes coming back
    // is what says the verbatim arm was not taken.
    CHECK(StatusOf(SyncRun(fix.proxy.Answer(Wire::EncodeFetch("k-foreign")))) == Wire::Status::Miss);

    // Counted, per this tier's own classification rule: the baseline is zero, so a
    // rise is a real event and it is the only view of what refusing costs.
    CHECK(fix.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedForeignGeneration) == 1);
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

    auto const compile = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "t",
                                                                    .fingerprint = "gcc-14",
                                                                    .args = {},
                                                                    .source = {},
                                                                    .acceptedCodecs = {},
                                                                    .sourceName = "t.cpp",
                                                                    .compileDir = {},
                                                                    .compileDirReplacement = {} });
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

TEST_CASE("The node's cache answers this machine and refuses every other one", "[node][cache][cache-locality]")
{
    // #287. This surface serves THIS MACHINE, always -- not "this machine and the
    // members an operator listed", and not "whatever the bind lets through".
    //
    // Making locality a property of the verb is what survives the surfaces being
    // merged onto one wildcard listener: after that, "it is only bound to loopback"
    // stops being available as an argument, and a rule that depended on it would
    // quietly stop being a rule.
    Fixture fixture;
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7", "fe80::1" } };
    CachedLocalityOracle const locality { machine, fixture.clock };
    CacheResponder responder { fixture.proxy, locality, fixture.metrics };

    auto const fetch = Wire::EncodeFetch("some-key");
    auto const ask = [&](std::string peer) {
        return Wire::DecodeReplyHeader(SyncRun(responder.Answer(fetch, std::move(peer))));
    };

    SECTION("over loopback, which is every ordinary fastcache-cc on this box")
    {
        // A miss, since nothing is stored: the request reached the tier and was
        // answered on its merits, which is what "served" looks like here.
        auto const reply = ask("127.0.0.1");
        REQUIRE(reply.has_value());
        CHECK(Unwrap(reply).status == Wire::Status::Miss);
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 0);
    }

    SECTION("over this machine's own routable address, on a widened bind")
    {
        // Loopback is not the whole of "this machine". A client on this host
        // dialling the node at the address the node advertises is still local, and
        // refusing it would break the deployment this tightening is supposed to
        // leave alone.
        auto const reply = ask("10.0.0.7");
        REQUIRE(reply.has_value());
        CHECK(Unwrap(reply).status == Wire::Status::Miss);
    }

    SECTION("over the IPv4-mapped spelling of that same address")
    {
        // A surface bound to `::` reports an IPv4 caller as `::ffff:10.0.0.7` while
        // the machine's interface list says `10.0.0.7`. A raw string compare would
        // refuse this machine's own clients on exactly the dual-stack bind this rule
        // was written for -- the failure #180 already paid for once on the member
        // list.
        auto const reply = ask("::ffff:10.0.0.7");
        REQUIRE(reply.has_value());
        CHECK(Unwrap(reply).status == Wire::Status::Miss);
    }

    SECTION("and refuses a machine that is not this one")
    {
        // Refused as a *reply*, never by closing: a client that cannot tell a policy
        // refusal from a dead host retries forever and reports a flaky network.
        auto const reply = ask("10.9.9.9");
        REQUIRE(reply.has_value());
        CHECK(Unwrap(reply).status == Wire::Status::Error);
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 1);
    }

    SECTION("and refuses a peer it cannot name at all")
    {
        // What `FormatPeerAddress` answers for a family it does not know or a
        // `getpeername` that failed. Two unanswerable questions are not a match, and
        // an empty entry in the address set must not become a wildcard.
        auto const reply = ask("");
        REQUIRE(reply.has_value());
        CHECK(Unwrap(reply).status == Wire::Status::Error);
    }
}

TEST_CASE("(#287) a fleet peer is refused this machine's cache tier, member or not",
          "[node][cache][membership][cache-locality]")
{
    // THE HOLE THIS CLOSED. `CacheResponder` used to gate on `Membership::Member`,
    // and a Member is any machine the operator listed -- so a peer on ANOTHER host
    // read this machine's entire build output. The docs promised exactly that
    // ("its own machine and its cluster"), which is why #287 is a deliberate
    // tightening rather than a bug fix, and why it shipped as a breaking change.
    //
    // The peer here is ADMITTED by the member list, and that is the whole point of
    // the fixture: a case whose caller was refused for some OTHER reason would pass
    // under the bug and prove nothing about locality.
    Fixture fixture;
    Distributed::ClusterMembership const membership { { "10.0.0.1:7000" } };
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, fixture.clock };
    CacheResponder responder { fixture.proxy, locality, fixture.metrics };

    // Stated first, so a later change to the membership vocabulary cannot turn this
    // case green by quietly reclassifying the peer: it IS a member, and it is
    // refused anyway.
    REQUIRE(membership.Classify("10.0.0.1") == Distributed::Membership::Member);

    // And it is not this machine, by either of the two properties that could make
    // it one.
    REQUIRE_FALSE(IsLoopbackHost("10.0.0.1"));
    REQUIRE_FALSE(locality.IsThisMachine("10.0.0.1"));

    auto const refused = SyncRun(responder.Answer(Wire::EncodeFetch("some-key"), "10.0.0.1"));
    auto const header = Wire::DecodeReplyHeader(refused);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).status == Wire::Status::Error);

    // `NotAMember` rather than a code of its own: `fastcache-cc` reads a FETCH
    // outcome as "is this daemon worth a second command" and steps over this one, so
    // a peer whose access was withdrawn compiles locally rather than failing. A new
    // code would be an unknown one to every launcher already deployed.
    CHECK(ErrorOf(refused) == Wire::ErrorCode::NotAMember);

    // And it is countable. The tightening withdrew access a fleet peer had, so an
    // operator whose peers stopped getting hits needs one number that says why.
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 1);
}

TEST_CASE("(#377) the cache's locality gate is one predicate, asked before the payload",
          "[node][cache][membership][cache-locality]")
{
    // #285/#377 moved this decision ahead of the payload read, which means it is now
    // reachable by two routes: `FrameServer` asks `RefusePeer` from the header, and
    // `Answer` asks it again for the callers that hand over a whole frame. Two routes
    // to one policy is how a gate drifts, and this pins that they cannot -- `Answer`
    // DELEGATES rather than restating the rule.
    Fixture fixture;
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, fixture.clock };
    CacheResponder responder { fixture.proxy, locality, fixture.metrics };

    SECTION("a stranger is refused from the header alone, and counted exactly once")
    {
        // The early route, with no frame in hand at all: this is what the server can
        // ask before it has read a byte of payload.
        auto const refusal = responder.RefusePeer("10.0.0.1", static_cast<std::uint8_t>(Wire::Op::Fetch));
        REQUIRE(refusal.has_value());
        CHECK(ErrorOf(Unwrap(refusal)) == Wire::ErrorCode::NotAMember);

        // ONE, not two. The increment lives inside the predicate, so a `FrameServer`
        // that asked early and an `Answer` that re-derived the rule would each charge
        // it -- and the counter an operator reads to explain lost hits would report
        // double the refusals that happened.
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 1);
    }

    SECTION("and this machine is not refused, so the early gate is not a closed door")
    {
        // The control: a predicate that refused everyone would satisfy the section
        // above while making the node's own cache unreachable, which is the failure
        // #229 already paid for.
        CHECK_FALSE(responder.RefusePeer("10.0.0.7", static_cast<std::uint8_t>(Wire::Op::Fetch)).has_value());
        CHECK_FALSE(responder.RefusePeer("127.0.0.1", static_cast<std::uint8_t>(Wire::Op::Fetch)).has_value());
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeCacheRequestsRefusedNotLocal) == 0);
    }
}

namespace
{
/// Every counter's reading, so a case can say what did NOT move.
///
/// A refusal test that checks only the wire code passes with every arm wired to one
/// shared counter -- and that is not hypothetical here: the very test written to prove
/// the two `MalformedFrame` refusals are separate passed on its first run while the
/// fixture was moving the neighbouring counter. So each case below asserts the whole
/// vector, not the one row it is about.
/// @param metrics The sink to read.
/// @return One reading per enumerator, in enumerator order.
[[nodiscard]] std::vector<std::uint64_t> AllCounters(AtomicMetricsSink const& metrics)
{
    std::vector<std::uint64_t> readings;
    readings.reserve(static_cast<std::size_t>(IMetricsSink::Counter::Last));
    for (auto const idx: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(IMetricsSink::Counter::Last)))
        readings.push_back(metrics.Read(static_cast<IMetricsSink::Counter>(idx)));
    return readings;
}

/// Which counters changed between @p before and @p after.
/// @param before A reading from `AllCounters`.
/// @param after A later reading from the same sink.
/// @return The enumerator indices whose readings differ, ascending.
[[nodiscard]] std::vector<std::size_t> Moved(std::vector<std::uint64_t> const& before,
                                             std::vector<std::uint64_t> const& after)
{
    std::vector<std::size_t> moved;
    for (auto const idx: std::views::iota(std::size_t { 0 }, before.size()))
        if (before[idx] != after[idx])
            moved.push_back(idx);
    return moved;
}

/// The single-element answer `Moved` gives when exactly @p counter rose.
/// @param counter The one enumerator expected to have moved.
/// @return That enumerator's index, as a one-element vector.
[[nodiscard]] std::vector<std::size_t> Only(IMetricsSink::Counter counter)
{
    return { static_cast<std::size_t>(counter) };
}
} // namespace

TEST_CASE("(#491) the cache surface counts the frame ceiling and the byte budget", "[node][cache][metrics]")
{
    // #491. The merged listener routes each refusal to the component owning the verb,
    // and for cache verbs the component answered correctly and moved nothing -- so an
    // operator alerting on the compile surface's series watched a flat graph while a
    // client hammered the port with 24-byte oversized declarations. That is #326's
    // scenario one surface over, and these are the two arms it fires through.
    Fixture fixture;
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, fixture.clock };
    CacheResponder const responder { fixture.proxy, locality, fixture.metrics };
    auto const fetch = static_cast<std::uint8_t>(Wire::Op::Fetch);

    SECTION("the frame ceiling, which is the cheapest probe there is")
    {
        auto const before = AllCounters(fixture.metrics);
        auto const reply = responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, fetch, "too big");

        // The client is told exactly what it was told before: this changes what an
        // operator can see, not what a launcher does.
        CHECK(ErrorOf(reply) == Wire::ErrorCode::PayloadTooLarge);
        CHECK(Moved(before, AllCounters(fixture.metrics))
              == Only(IMetricsSink::Counter::NodeCacheRequestsRefusedPayloadTooLarge));
    }

    SECTION("the in-flight byte budget, which is the one a node with a tier reaches")
    {
        // `MergedResponder::MaxInFlightBytes()` folds to the LARGEST owner's budget,
        // which on any node holding a cache tier is this cache's -- so the endpoint's
        // ceiling is this cache's ceiling, and a `STORE` is what runs into it.
        auto const before = AllCounters(fixture.metrics);
        auto const reply = responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, fetch, "full");

        CHECK(ErrorOf(reply) == Wire::ErrorCode::EndpointBusy);
        CHECK(Moved(before, AllCounters(fixture.metrics))
              == Only(IMetricsSink::Counter::NodeCacheRequestsRefusedEndpointBusy));
    }

    SECTION("and neither is summed into another surface's row for the same wire code")
    {
        // Three refusals answer `endpoint-busy` on this listener and two answer
        // `payload-too-large`. A client does the same thing about each pair and an
        // operator does not, which is the whole argument for a row being the refusal
        // rather than the code -- so the separation is asserted rather than assumed.
        (void) responder.RefusalReply(Wire::PrePayloadDecision::PayloadTooLarge, fetch, {});
        (void) responder.EndpointRefusalReply(EndpointRefusal::InFlightBudget, fetch, {});

        CHECK(fixture.metrics.Read(IMetricsSink::Counter::WorkerFramesRefusedPayloadTooLarge) == 0);
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedEndpointBusy) == 0);
        CHECK(fixture.metrics.Read(IMetricsSink::Counter::NodeFrameConnectionsRefusedAtCapacity) == 0);
    }
}

TEST_CASE("(#491) the cache tier counts a version skew and an undecodable body", "[node][cache][metrics]")
{
    SECTION("a request at a version this build cannot decode")
    {
        // Worth its own series because the only other evidence is a cache that looks
        // permanently cold: the launcher steps over a refused FETCH and compiles
        // locally, so the build stays correct and merely stops being fast.
        Fixture fix;
        std::array<std::byte, Wire::RequestHeaderSize> frame {};
        WireFrame::PutHeader(frame,
                             Wire::Magic,
                             static_cast<Wire::WireVersion>(Wire::CurrentVersion + 1),
                             static_cast<std::uint8_t>(Wire::Op::Fetch),
                             0);

        auto const before = AllCounters(fix.metrics);
        CHECK(ErrorOf(SyncRun(fix.proxy.Answer(frame))) == Wire::ErrorCode::UnsupportedVersion);
        CHECK(Moved(before, AllCounters(fix.metrics))
              == Only(IMetricsSink::Counter::NodeCacheRequestsRefusedUnsupportedVersion));
    }

    SECTION("a FETCH or a STORE whose body will not decode reaches one row")
    {
        // One row for both verbs: they carry different fields and say the same thing
        // about the peer, and an operator does one thing about it. Driven over the two
        // verbs rather than written twice, because a second copy of this frame is a
        // second place for the length below to be got wrong.
        //
        // The declared length MATCHES the bytes sent, deliberately. Shortened instead,
        // this would be refused as TRUNCATED -- answering the same wire code and
        // moving a different counter -- which is exactly how the compile surface's
        // equivalent test passed under the bug it was written to catch.
        for (auto const op: { Wire::Op::Fetch, Wire::Op::Store })
        {
            INFO("verb " << static_cast<unsigned>(op));
            Fixture fix;
            std::vector<std::byte> frame(Wire::RequestHeaderSize + 2);
            WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, static_cast<std::uint8_t>(op), 2);
            // A length prefix claiming far more field than the payload can hold.
            frame[Wire::RequestHeaderSize] = std::byte { 0xFF };
            frame[Wire::RequestHeaderSize + 1] = std::byte { 0xFF };

            auto const before = AllCounters(fix.metrics);
            CHECK(ErrorOf(SyncRun(fix.proxy.Answer(frame))) == Wire::ErrorCode::MalformedFrame);
            CHECK(Moved(before, AllCounters(fix.metrics))
                  == Only(IMetricsSink::Counter::NodeCacheRequestsRefusedMalformedPayload));
        }
    }

    SECTION("and the AUTH refusal a token-configured launcher gets every exchange moves nothing")
    {
        // Deliberately uncounted, and the arm where a counter would be actively
        // harmful: a `FASTCACHE_TOKEN` launcher sends AUTH once per exchange for a
        // whole build, so the series would be dominated by healthy traffic and a port
        // scan invisible inside it. The wire code still has to be exactly right --
        // `unimplemented-verb` is the one refusal `Cc::CacheProtocol` steps over
        // rather than treating as fatal.
        Fixture fix;
        auto const before = AllCounters(fix.metrics);
        auto const reply =
            SyncRun(fix.proxy.Answer(Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "s3cret" })));

        CHECK(ErrorOf(reply) == Wire::UnimplementedVerb);
        CHECK(Moved(before, AllCounters(fix.metrics)).empty());
    }
}

TEST_CASE("(#491) the cache surface's uncounted arms are unreachable, swept rather than asserted", "[node][cache][metrics]")
{
    // Four arms here are uncounted BECAUSE nothing can reach them, which is a claim
    // about routing rather than about policy -- and a claim about routing is exactly
    // the kind that stops being true without anybody editing the sentence stating it.
    // So it is measured over every byte value a peer can put in the third header byte,
    // and that sweep is what makes this surface's answer defensible where
    // `CompileCapacity`'s credential arms take the opposite line and mint counters
    // that cannot rise.
    Fixture fixture;
    Testing::ScriptedHostAddresses const machine { { "10.0.0.7" } };
    CachedLocalityOracle const locality { machine, fixture.clock };
    CacheResponder cache { fixture.proxy, locality, fixture.metrics };
    MergedResponder const merged { &cache, nullptr, nullptr };

    SECTION("no opcode routes an unknown verb to the cache, so its UnknownOpcode arm cannot fire")
    {
        // `FamilyOf` gives an opcode with no `OpTable` row the `Unset` family, which
        // `MergedResponder` owns nowhere. "Reached this tier" and "has no row" are
        // therefore mutually exclusive by the definition of `FamilyOf` rather than by
        // a routing decision somebody could revisit.
        //
        // Both directions, because the one-directional half would pass for a router
        // that sent NOTHING to the cache -- which is a broken node, not a proof.
        auto served = 0;
        for (auto const value: std::views::iota(0, 256))
        {
            auto const opRaw = static_cast<std::uint8_t>(value);
            INFO("opcode " << value);
            CHECK((merged.OwnerOf(opRaw) == &cache) == (Wire::FamilyOf(opRaw) == Wire::VerbFamily::Cache));
            if (merged.OwnerOf(opRaw) == &cache)
            {
                CHECK(Wire::FindOp(opRaw) != nullptr);
                ++served;
            }
        }
        // FETCH and STORE. Named as a floor rather than an equality so adding a cache
        // verb does not fail this, but a router that stopped serving any does.
        CHECK(served >= 2);
    }

    SECTION("no opcode reaches the cache's Unauthenticated arm, because it requires no credential")
    {
        // `AuthRequired()` is false here by decision (#287, #290): a credential every
        // local build can read is not a credential. `DecidePrePayload` yields
        // `Unauthenticated` only for a surface that requires one, so this arm is
        // closed by that answer rather than by the routing above.
        for (auto const value: std::views::iota(0, 256))
        {
            auto const opRaw = static_cast<std::uint8_t>(value);
            INFO("opcode " << value);
            CHECK_FALSE(cache.AuthRequired(opRaw));
            CHECK(Wire::DecidePrePayload({ .opRaw = opRaw,
                                           .declaredLength = 0,
                                           .sessionCap = cache.MaxRequestBytes(),
                                           .authRequired = cache.AuthRequired(opRaw),
                                           .credentialAccepted = false })
                  != Wire::PrePayloadDecision::Unauthenticated);
        }
    }

    SECTION("AUTH is the Session family, so no credential outcome is ever decided against the cache")
    {
        // The two `EndpointRefusal` credential arms are answered by whichever surface
        // owns `Op::Auth`. With no scheduler configured that is NOBODY -- and the
        // point of asserting the null is that a router which fell back to the cache
        // would answer `&cache` here and reopen both arms in silence.
        auto const auth = static_cast<std::uint8_t>(Wire::Op::Auth);
        CHECK(Wire::FamilyOf(auth) == Wire::VerbFamily::Session);
        CHECK(merged.OwnerOf(auth) == nullptr);
    }
}
