// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cache/StorageTier.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Distributed;

using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// A caller the fleet has admitted.
constexpr CallerContext Insider { .membership = Membership::Member, .peerId = "peer-1" };

/// A scheduler that already leads, plus the protocol in front of it.
struct Fixture
{
    Fixture()
    {
        service.SetRole(SchedulerRole::Leader, {});
    }

    ManualClock clock;
    AtomicMetricsSink metrics;
    SchedulerService service { clock, metrics };
    SchedulerProtocol protocol { service };
};

/// The status byte of a reply, or nullopt when the reply is unreadable.
[[nodiscard]] std::optional<Wire::Status> StatusOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    return header.has_value() ? std::optional { header->status } : std::nullopt;
}

/// The error code of a refusal, or nullopt when the reply is not one.
[[nodiscard]] std::optional<Wire::ErrorCode> ErrorOf(std::span<std::byte const> reply)
{
    auto const header = Wire::DecodeReplyHeader(reply);
    if (!header.has_value() || header->status != Wire::Status::Error || header->payloadLength == 0)
        return std::nullopt;
    return static_cast<Wire::ErrorCode>(reply[Wire::ReplyHeaderSize]);
}

/// The payload of a reply, as bytes.
[[nodiscard]] std::span<std::byte const> PayloadOf(std::span<std::byte const> reply)
{
    return reply.subspan(Wire::ReplyHeaderSize);
}
} // namespace

TEST_CASE("A scheduler answers its own verbs and nothing else", "[distributed][scheduler][protocol]")
{
    // The refusal is a *reply* and not a close: a client that sent a cache verb to
    // the scheduler's port learns which, rather than seeing a dropped connection it
    // cannot tell from a dead host. `WorkerProtocol` records the same reasoning
    // pointing the other way.
    Fixture fixture;

    SECTION("STORE is refused rather than served")
    {
        auto const frame = Wire::EncodeStore(
            Wire::StoreRequest { .key = "k", .prefetchGroup = {}, .srcRoot = "/src", .buildTree = "/build", .value = {} });
        auto const reply = fixture.protocol.Answer(frame, Insider);
        CHECK(StatusOf(reply) == Wire::Status::Error);
        CHECK(ErrorOf(reply) == Wire::ErrorCode::DispatchNotPermitted);
    }

    SECTION("FETCH likewise")
    {
        auto const reply = fixture.protocol.Answer(Wire::EncodeFetch("k"), Insider);
        CHECK(ErrorOf(reply) == Wire::ErrorCode::DispatchNotPermitted);
    }

    SECTION("COMPILE is a worker's verb, not a scheduler's")
    {
        // The scheduler and the worker share a wire and answer disjoint halves of
        // it. Asserted because "the scheduler also runs compiles" is the obvious
        // wrong turn, and the refusal is what says so out loud.
        auto const frame = Wire::EncodeCompile(Wire::CompileRequest { .leaseToken = "t",
                                                                      .fingerprint = "gcc-14",
                                                                      .args = {},
                                                                      .source = {},
                                                                      .acceptedCodecs = {},
                                                                      .sourceName = "t.cpp" });
        CHECK(ErrorOf(fixture.protocol.Answer(frame, Insider)) == Wire::ErrorCode::DispatchNotPermitted);
    }
}

TEST_CASE("An unknown opcode is stepped over, not fatal", "[distributed][scheduler][protocol]")
{
    // The framing exists so a receiver can skip a verb it does not know, which is
    // what lets a newer client talk to an older scheduler at all. A close here
    // would make every mixed-version fleet look like a flaky network.
    Fixture fixture;

    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, Wire::Magic, Wire::CurrentVersion, /*kindRaw=*/0xEE, /*payloadLength=*/0);

    auto const reply = fixture.protocol.Answer(frame, Insider);
    CHECK(StatusOf(reply) == Wire::Status::Error);
    CHECK(ErrorOf(reply) == Wire::ErrorCode::UnknownOpcode);
}

TEST_CASE("A version outside the range is told what would have worked", "[distributed][scheduler][protocol]")
{
    // A rejection that cannot say what would have worked cannot be acted on -- the
    // rule the whole `[magic][version][op][length]` header was introduced for.
    Fixture fixture;

    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(
        frame, Wire::Magic, static_cast<Wire::WireVersion>(0xFE), static_cast<std::uint8_t>(Wire::Op::Lease), 0);

    auto const reply = fixture.protocol.Answer(frame, Insider);
    REQUIRE(ErrorOf(reply) == Wire::ErrorCode::UnsupportedVersion);

    auto const payload = PayloadOf(reply).subspan(1);
    auto const text = std::string { reinterpret_cast<char const*>(payload.data()), payload.size() };
    CHECK(text.contains("supported versions"));
}

TEST_CASE("A frame that is not this protocol is the one condition that closes", "[distributed][scheduler][protocol]")
{
    // Wrong magic means the peer is not speaking this protocol at all, and with no
    // declared length there is no way to find where the frame ended -- so there is
    // nothing to resynchronize to and no way to answer. Every other refusal is a
    // reply; this one is an empty answer, which is how the transport is told to
    // close.
    Fixture fixture;

    std::array<std::byte, Wire::RequestHeaderSize> frame {};
    WireFrame::PutHeader(frame, /*magic=*/std::byte { 0x11 }, Wire::CurrentVersion, 0x01, 0);

    CHECK(fixture.protocol.Answer(frame, Insider).empty());
}

TEST_CASE("A payload that does not fill its declared length is refused", "[distributed][scheduler][protocol]")
{
    Fixture fixture;

    auto frame = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k", .acceptedCodecs = {} });
    frame.pop_back(); // the header still declares the original length

    CHECK(ErrorOf(fixture.protocol.Answer(frame, Insider)) == Wire::ErrorCode::MalformedFrame);

    // And it moves no counter, which is the half that used to be asserted a layer
    // down. A malformed frame is a CLIENT defect: counting it beside the capacity
    // refusals would put one broken build's noise into the numbers a fleet is sized
    // from, which is the reason `RefusalTable` lets a row name no counter at all.
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoWorker) == 0);
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 0);
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchLeasesDuplicate) == 0);
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchWorkerRegistrations) == 0);
}

TEST_CASE("A whole register-heartbeat-lease-release exchange crosses the wire", "[distributed][scheduler][protocol]")
{
    // End to end through the framing, because the service's own tests call it
    // directly and would not notice a field read out of order here.
    Fixture fixture;

    auto const registration = Wire::EncodeRegister(
        Wire::RegisterRequest { .fingerprint = "gcc-14", .endpoint = "10.0.0.2:7100", .slots = 2, .acceptedCodecs = {} });
    auto const admitted = fixture.protocol.Answer(registration, Insider);
    REQUIRE(StatusOf(admitted) == Wire::Status::Ok);
    auto const workerId = std::string { Wire::AsStringView(PayloadOf(admitted)) };
    REQUIRE_FALSE(workerId.empty());

    auto const beat = Wire::EncodeHeartbeat(workerId, /*inFlight=*/0);
    CHECK(StatusOf(fixture.protocol.Answer(beat, Insider)) == Wire::Status::Ok);

    auto const ask = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k1", .acceptedCodecs = {} });
    auto const granted = fixture.protocol.Answer(ask, Insider);
    REQUIRE(StatusOf(granted) == Wire::Status::Ok);

    auto const grant = Wire::DecodeLeaseGrant(PayloadOf(granted));
    REQUIRE(grant.has_value());
    CHECK(Wire::AsStringView(Unwrap(grant).endpoint) == "10.0.0.2:7100");

    // And back again, which is the transition that had no verb at all: the token
    // the grant carried is what resolves the lease, so a field read out of order in
    // either encoder would leave a key pinned until it expired (#212).
    auto const done = Wire::EncodeRelease(
        Wire::ReleaseRequest { .leaseToken = Wire::AsStringView(Unwrap(grant).leaseToken), .key = "k1" });
    CHECK(StatusOf(fixture.protocol.Answer(done, Insider)) == Wire::Status::Ok);

    // Spent, so the same token now names nothing.
    CHECK(ErrorOf(fixture.protocol.Answer(done, Insider)) == Wire::ErrorCode::UnknownLease);
}

TEST_CASE("The service's refusals reach the wire with their codes intact", "[distributed][scheduler][protocol]")
{
    // The point of typed codes: a client acts differently on each, and a protocol
    // layer that collapsed them into one "no" would undo that silently.
    Fixture fixture;

    auto const ask = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k1", .acceptedCodecs = {} });

    SECTION("an empty fleet")
    {
        CHECK(ErrorOf(fixture.protocol.Answer(ask, Insider)) == Wire::ErrorCode::NoWorker);
    }

    SECTION("a non-member")
    {
        constexpr CallerContext outsider { .membership = Membership::Outsider, .peerId = "stranger" };
        CHECK(ErrorOf(fixture.protocol.Answer(ask, outsider)) == Wire::ErrorCode::NotAMember);
    }

    SECTION("a follower, carrying the leader's endpoint in the message")
    {
        fixture.service.SetRole(SchedulerRole::Follower, "10.0.0.1:7000");
        auto const reply = fixture.protocol.Answer(ask, Insider);
        REQUIRE(ErrorOf(reply) == Wire::ErrorCode::NotLeader);

        auto const payload = PayloadOf(reply).subspan(1);
        CHECK(std::string { reinterpret_cast<char const*>(payload.data()), payload.size() } == "10.0.0.1:7000");
    }
}

TEST_CASE("The two halves of the capacity mapping agree", "[distributed][scheduler][protocol]")
{
    // They are one mapping written twice — the node encodes, the scheduler decodes —
    // and the failure that invites is a transposition: cores read as memory, a
    // reserve read as a class. Every field is a distinct value, because two fields
    // sharing one is exactly what lets a transposition through.
    constexpr NodeCapacity original { .logicalCores = 24,
                                      .totalMemoryBytes = 137438953472,
                                      .nodeClass = NodeClass::Dedicated,
                                      .reservedCores = 5,
                                      .reserveIsExplicit = true };

    auto const back = CapacityFromWire(CapacityToWire(original));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).logicalCores == original.logicalCores);
    CHECK(Unwrap(back).totalMemoryBytes == original.totalMemoryBytes);
    CHECK(Unwrap(back).nodeClass == original.nodeClass);
    CHECK(Unwrap(back).reservedCores == original.reservedCores);
    CHECK(Unwrap(back).reserveIsExplicit);
}

TEST_CASE("An unstated reserve survives as unstated, not as zero", "[distributed][scheduler][protocol]")
{
    // The distinction the whole optional exists for. A node that never mentioned a
    // reserve must arrive as "use whatever the class reserves"; arriving as zero
    // would drive somebody's desktop to its last core with nothing anywhere saying
    // so, which is the exact failure `NodeClass`'s zero value is chosen to prevent.
    constexpr NodeCapacity quiet { .logicalCores = 16 };

    auto const back = CapacityFromWire(CapacityToWire(quiet));
    REQUIRE(back.has_value());
    CHECK_FALSE(Unwrap(back).reserveIsExplicit);
    CHECK(OfferableSlots(Unwrap(back), 0) == 14);
}

TEST_CASE("A registration naming a node class this build lacks is refused", "[distributed][scheduler][protocol]")
{
    // Refused rather than clamped, and answered rather than dropped: the peer learns
    // which of the two builds is behind. Reading it as `Workstation` would offer a
    // dedicated build server two cores fewer than it has, and reading it as
    // `Dedicated` would saturate somebody's desk — neither visible from either end.
    Fixture fixture;

    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest {
        .fingerprint = "gcc-14",
        .endpoint = "10.0.0.2:7100",
        .slots = 0,
        .acceptedCodecs = {},
        .capacity = Wire::CapacityFields {
            .logicalCores = 8, .totalMemoryBytes = 0, .nodeClassRaw = 200, .reservedCores = std::nullopt } });

    CHECK(ErrorOf(fixture.protocol.Answer(frame, Insider)) == Wire::ErrorCode::MalformedFrame);
}

TEST_CASE("A node that names no slot count is sized from the facts it sent", "[distributed][scheduler][protocol]")
{
    // End to end through the wire rather than against the policy function directly:
    // this is the path that would silently produce a full-capacity workstation if the
    // capacity field were dropped anywhere between the two.
    Fixture fixture;

    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest {
        .fingerprint = "gcc-14",
        .endpoint = "10.0.0.2:7100",
        .slots = 0,
        .acceptedCodecs = {},
        .capacity = Wire::CapacityFields {
            .logicalCores = 8, .totalMemoryBytes = 64ULL << 30, .nodeClassRaw = 0, .reservedCores = std::nullopt } });
    REQUIRE(StatusOf(fixture.protocol.Answer(frame, Insider)) == Wire::Status::Ok);

    auto const live = fixture.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    CHECK(live[0].slots == 6); // eight cores, less the workstation reserve of two
}

TEST_CASE("The two halves of the load mapping agree", "[distributed][scheduler][protocol]")
{
    // Same reasoning as the capacity pair, and the risk is sharper here: two of the
    // three fields are the same width, so swapping memory for scratch space is a
    // transposition that decodes perfectly and produces a scheduler that withdraws
    // machines for the wrong reason.
    constexpr NodeLoad original {
        .inFlight = 6, .cpuBusyPermille = 640, .availableMemoryBytes = 8589934592, .freeScratchBytes = 274877906944
    };

    auto const back = LoadFromWire(LoadToWire(original), original.inFlight);
    CHECK(back.inFlight == original.inFlight);
    CHECK(back.cpuBusyPermille == original.cpuBusyPermille);
    CHECK(back.availableMemoryBytes == original.availableMemoryBytes);
    CHECK(back.freeScratchBytes == original.freeScratchBytes);
}

TEST_CASE("A heartbeat's load reaches the registry and changes what is picked", "[distributed][scheduler][protocol]")
{
    // End to end through the framing. Every layer between the worker and `Pick`
    // could drop this silently -- the encoder, the decoder, the mapping, the
    // registry's own store -- and the symptom of any of them would be a fleet that
    // keeps sending work to a machine that told it not to.
    Fixture fixture;

    auto const registration = Wire::EncodeRegister(Wire::RegisterRequest {
        .fingerprint = "gcc-14",
        .endpoint = "10.0.0.2:7100",
        .slots = 0,
        .acceptedCodecs = {},
        .capacity = Wire::CapacityFields {
            .logicalCores = 8, .totalMemoryBytes = 0, .nodeClassRaw = 1, .reservedCores = std::nullopt } });
    REQUIRE(StatusOf(fixture.protocol.Answer(registration, Insider)) == Wire::Status::Ok);

    auto const live = fixture.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    REQUIRE(live[0].slots == 8);

    auto const ask = Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k1", .acceptedCodecs = {} });
    REQUIRE(StatusOf(fixture.protocol.Answer(ask, Insider)) == Wire::Status::Ok);

    // The worker now says its scratch filesystem is full. Nothing about the fleet
    // has changed except what it was told.
    auto const beat = Wire::EncodeHeartbeat(
        live[0].id,
        0,
        Wire::LoadFields { .cpuBusyPermille = std::nullopt, .availableMemoryBytes = std::nullopt, .freeScratchBytes = 0 });
    REQUIRE(StatusOf(fixture.protocol.Answer(beat, Insider)) == Wire::Status::Ok);

    // `Withdrawn`, not `NoCapacity`: the worker has seven of its eight slots free and
    // is refusing to use them. Reported as no-capacity, this would send an operator
    // to buy machines when the fix is 200 MB of disk on one they own.
    auto const refused = fixture.protocol.Answer(
        Wire::EncodeLease(Wire::LeaseRequest { .fingerprint = "gcc-14", .key = "k2", .acceptedCodecs = {} }), Insider);
    CHECK(ErrorOf(refused) == Wire::ErrorCode::Withdrawn);
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchLeasesWithdrawn) == 1);
    CHECK(fixture.metrics.Read(IMetricsSink::Counter::DispatchLeasesNoCapacity) == 0);
}

TEST_CASE("The two halves of the cache-capacity mapping agree", "[distributed][scheduler][protocol][cache]")
{
    // The transposition hazard here is of a different kind from cores and memory:
    // the wire carries tiers by POSITION, because the wire header is compiled into
    // `fastcache-cc` and cannot see `StorageTier`. Nothing but these two functions
    // knows that index 1 is the disk tier, so a swap between them decodes perfectly
    // and reports a node's RAM budget as its disk budget. A distinct value per tier
    // is what makes such a swap visible at all.
    NodeCacheCapacity original {};
    original.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 256ULL << 20;
    original.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] = 40ULL << 30;

    auto const back = CacheCapacityFromWire(CacheCapacityToWire(original));
    CHECK(back.tierBytesLimit == original.tierBytesLimit);
    CHECK(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] == 256ULL << 20);
    CHECK(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] == 40ULL << 30);
}

TEST_CASE("A node with no disk tier does not grow one on the wire", "[distributed][scheduler][protocol][cache]")
{
    // Absent is not zero, and a memory-only node is the default deployment. A disk
    // budget of zero would be read as "on disk, unbounded", which is the opposite of
    // what such a node is doing.
    NodeCacheCapacity memoryOnly {};
    memoryOnly.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 1024;

    auto const back = CacheCapacityFromWire(CacheCapacityToWire(memoryOnly));
    CHECK(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)].has_value());
    CHECK_FALSE(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)].has_value());

    // And a node with no cache at all stays that way through both directions.
    auto const none = CacheCapacityFromWire(CacheCapacityToWire(NodeCacheCapacity {}));
    for (auto const& tier: none.tierBytesLimit)
        CHECK_FALSE(tier.has_value());
}

TEST_CASE("A configured-but-unbounded tier is not the same as no tier", "[distributed][scheduler][protocol][cache]")
{
    // The pair the optional exists to keep apart. A tier configured with no ceiling
    // reports a budget of zero, which reads as "unbounded"; a node without that tier
    // reports nothing at all. Flattening either into the other has a dashboard
    // stating a fact the fleet never sent.
    NodeCacheCapacity unbounded {};
    unbounded.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] = 0;

    auto const back = CacheCapacityFromWire(CacheCapacityToWire(unbounded));
    REQUIRE(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)].has_value());
    CHECK(Unwrap(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)]) == 0);
    CHECK_FALSE(back.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)].has_value());
}

TEST_CASE("The two halves of the cache-load mapping agree", "[distributed][scheduler][protocol][cache]")
{
    // Every value distinct across both tiers and all FOUR fields, for the reason the
    // capacity case gives: same-width numbers per tier make a transposition decode
    // perfectly, and the fourth arrived last (#175) precisely where a renumbering
    // would be invisible.
    NodeCacheLoad original {};
    original.tiers[static_cast<std::size_t>(StorageTier::Memory)] =
        CacheTierUsage { .itemCount = 11, .bytesUsed = 22, .evictions = 33, .indexBytes = 99 };
    original.tiers[static_cast<std::size_t>(StorageTier::Disk)] =
        CacheTierUsage { .itemCount = 44, .bytesUsed = 55, .evictions = 66, .indexBytes = 111 };
    original.hits = 77;
    original.misses = 88;

    auto const back = CacheLoadFromWire(CacheLoadToWire(original));
    auto const& memory = back.tiers[static_cast<std::size_t>(StorageTier::Memory)];
    auto const& disk = back.tiers[static_cast<std::size_t>(StorageTier::Disk)];
    REQUIRE(memory.has_value());
    REQUIRE(disk.has_value());
    CHECK(Unwrap(memory).itemCount == 11);
    CHECK(Unwrap(memory).bytesUsed == 22);
    CHECK(Unwrap(memory).evictions == 33);
    CHECK(Unwrap(disk).itemCount == 44);
    CHECK(Unwrap(disk).bytesUsed == 55);
    CHECK(Unwrap(disk).evictions == 66);
    // #175: the disk tier's key index, which is RAM its disk budget does not describe.
    CHECK(Unwrap(memory).indexBytes == 99);
    CHECK(Unwrap(disk).indexBytes == 111);
    CHECK(back.hits == 77);
    CHECK(back.misses == 88);
}

TEST_CASE("A cache-load record from a peer that predates indexBytes still decodes",
          "[distributed][scheduler][protocol][cache]")
{
    // The compatibility claim `indexBytes` was appended on (#175), asserted rather
    // than asserted-in-a-comment. This record's arity is variable by design: the
    // decoder stops at whichever side has fewer fields and leaves the rest at zero.
    // So a node built before the field sends three, and a leader built after it must
    // read those three correctly rather than refusing the heartbeat or shifting the
    // values along by one.
    //
    // Built by hand at the wire level, because there is no older binary to ask and a
    // round trip through this build's own encoder could never produce a short record.
    std::vector<std::vector<std::byte>> owned;
    std::vector<std::span<std::byte const>> tierFields;
    for (auto const& row: StorageTierTable)
    {
        static_cast<void>(row);
        owned.push_back(WireFields::Encode({ std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(7) },
                                             std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(8) },
                                             std::span<std::byte const> { WireFields::ToBigEndian<std::uint64_t>(9) } }));
        tierFields.emplace_back(owned.back());
    }

    auto const decoded = Wire::DecodeCacheLoad(WireFields::Encode(WireFields::FieldList { {
        std::span<std::byte const> { WireFields::Encode(WireFields::FieldList { tierFields }) },
    } }));
    REQUIRE(decoded.has_value());

    auto const& memory = Unwrap(decoded).tiers[static_cast<std::size_t>(StorageTier::Memory)];
    REQUIRE(memory.has_value());
    CHECK(Unwrap(memory).itemCount == 7);
    CHECK(Unwrap(memory).bytesUsed == 8);
    CHECK(Unwrap(memory).evictions == 9);

    // Absent stays zero, which is the only honest answer: a node that could not say
    // what its index costs is not a node whose index costs nothing, but zero is what
    // the renderer already treats as "nothing to show" for this figure.
    CHECK(Unwrap(memory).indexBytes == 0);
}

TEST_CASE("A capacity record carries the toolchain label, and tolerates one that predates it",
          "[distributed][scheduler][protocol]")
{
    // #194. The label rides the NESTED capacity record rather than REGISTER's top
    // level, because that level's arity is exact and an addition there makes two
    // builds unable to speak. This record tolerates a short peer, which is what makes
    // the field addable at all -- so both halves are asserted: that it survives a
    // round trip, and that a record written before it existed still decodes.
    Wire::CapacityFields wire {};
    wire.logicalCores = 8;
    wire.version = "0.2.0";
    wire.toolchainLabel = "cl 19.44.35207";

    auto const back = Wire::DecodeCapacity(Wire::EncodeCapacity(wire));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).toolchainLabel == "cl 19.44.35207");
    // Not confused with the field beside it: both are free-form strings in one
    // record, and a transposition would decode perfectly.
    CHECK(Unwrap(back).version == "0.2.0");

    // Absent is the honest answer for an operator's pinned `<fingerprint>=<compiler>`
    // override, which is never probed and so has no banner to read a label out of --
    // and for any node built before the field. Both arrive as empty, and empty is
    // rendered as absent rather than as a blank.
    Wire::CapacityFields quiet {};
    quiet.logicalCores = 4;
    auto const quietBack = Wire::DecodeCapacity(Wire::EncodeCapacity(quiet));
    REQUIRE(quietBack.has_value());
    CHECK(Unwrap(quietBack).toolchainLabel.empty());
    CHECK(Unwrap(quietBack).logicalCores == 4);
}

TEST_CASE("A node that will not state its hit rate is not a node with no hits", "[distributed][scheduler][protocol][cache]")
{
    NodeCacheLoad quiet {};
    quiet.tiers[static_cast<std::size_t>(StorageTier::Memory)] = CacheTierUsage { .itemCount = 5 };

    auto const back = CacheLoadFromWire(CacheLoadToWire(quiet));
    CHECK_FALSE(back.hits.has_value());
    CHECK_FALSE(back.misses.has_value());
    auto const& memory = back.tiers[static_cast<std::size_t>(StorageTier::Memory)];
    REQUIRE(memory.has_value());
    CHECK(Unwrap(memory).itemCount == 5);
}

TEST_CASE("The cache facts cross the wire inside REGISTER and HEARTBEAT", "[distributed][scheduler][protocol][cache]")
{
    // End to end through the framing rather than against the mappings alone. Every
    // layer between the node and the registry could drop this in silence -- the
    // encoder, the decoder, the nesting, the assignment into WorkerInfo -- and the
    // symptom would be a leader reporting that no member has a cache, which is
    // indistinguishable from a fleet of nodes that genuinely have none.
    Fixture fixture;

    NodeCacheCapacity budget {};
    budget.tierBytesLimit[static_cast<std::size_t>(StorageTier::Memory)] = 256ULL << 20;
    budget.tierBytesLimit[static_cast<std::size_t>(StorageTier::Disk)] = 8ULL << 30;

    auto const registerFrame = Wire::EncodeRegister(
        Wire::RegisterRequest { .fingerprint = "gcc-14",
                                .endpoint = "10.0.0.2:7100",
                                .slots = 0,
                                .acceptedCodecs = {},
                                .capacity = Wire::CapacityFields { .logicalCores = 8,
                                                                   .totalMemoryBytes = 64ULL << 30,
                                                                   .nodeClassRaw = 0,
                                                                   .reservedCores = std::nullopt,
                                                                   .cache = CacheCapacityToWire(budget) } });
    REQUIRE(StatusOf(fixture.protocol.Answer(registerFrame, Insider)) == Wire::Status::Ok);

    auto const registered = fixture.service.Workers().LiveWorkers();
    REQUIRE(registered.size() == 1);
    CHECK(registered[0].capacity.cache.tierBytesLimit == budget.tierBytesLimit);

    NodeCacheLoad usage {};
    usage.tiers[static_cast<std::size_t>(StorageTier::Memory)] =
        CacheTierUsage { .itemCount = 900, .bytesUsed = 100ULL << 20, .evictions = 12 };
    usage.hits = 4000;
    usage.misses = 100;

    auto const heartbeat = Wire::EncodeHeartbeat(
        std::string { registered[0].id },
        0,
        Wire::LoadFields {
            .cpuBusyPermille = 100, .availableMemoryBytes = 1, .freeScratchBytes = 2, .cache = CacheLoadToWire(usage) });
    REQUIRE(StatusOf(fixture.protocol.Answer(heartbeat, Insider)) == Wire::Status::Ok);

    auto const beating = fixture.service.Workers().LiveWorkers();
    REQUIRE(beating.size() == 1);
    auto const& memory = beating[0].load.cache.tiers[static_cast<std::size_t>(StorageTier::Memory)];
    REQUIRE(memory.has_value());
    CHECK(Unwrap(memory).itemCount == 900);
    CHECK(beating[0].load.cache.hits == 4000);
    CHECK(beating[0].load.cache.misses == 100);
    // The budget is a REGISTRATION fact and survives a heartbeat that says nothing
    // about it, which is the reason it does not travel on every one.
    CHECK(beating[0].capacity.cache.tierBytesLimit == budget.tierBytesLimit);
}

TEST_CASE("A peer that predates the cache record still registers", "[distributed][scheduler][protocol][cache]")
{
    // The property the nesting exists for. An older node sends a capacity record of
    // four fields and a load record of three; both must be accepted with the cache
    // left at "did not say" rather than refused, or a fleet mid-upgrade stops
    // distributing anything at all.
    Fixture fixture;

    auto const frame = Wire::EncodeRegister(Wire::RegisterRequest {
        .fingerprint = "gcc-14",
        .endpoint = "10.0.0.9:7100",
        .slots = 0,
        .acceptedCodecs = {},
        .capacity = Wire::CapacityFields {
            .logicalCores = 4, .totalMemoryBytes = 8ULL << 30, .nodeClassRaw = 0, .reservedCores = std::nullopt } });
    REQUIRE(StatusOf(fixture.protocol.Answer(frame, Insider)) == Wire::Status::Ok);

    auto const live = fixture.service.Workers().LiveWorkers();
    REQUIRE(live.size() == 1);
    for (auto const& tier: live[0].capacity.cache.tierBytesLimit)
        CHECK_FALSE(tier.has_value());
}

TEST_CASE("A node's version rides the capacity record and survives the round trip",
          "[distributed][scheduler][protocol][version]")
{
    // Inside the capacity record rather than at REGISTER's top level, because that
    // message's arity is exact and fixed forever -- a sixth field there would make
    // two builds of this fleet unable to speak at all.
    auto wire = CapacityToWire(NodeCapacity { .logicalCores = 8 });
    wire.version = "1.4.2-7-gdeadbee";

    auto const encoded = Wire::EncodeCapacity(wire);
    auto const back = Wire::DecodeCapacity(encoded);
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).version == "1.4.2-7-gdeadbee");
    // And the rest of the record still reads, which is the half a new trailing
    // field is most likely to break.
    CHECK(Unwrap(back).logicalCores == 8);
}

TEST_CASE("A peer that predates the version field registers rather than being refused",
          "[distributed][scheduler][protocol][version]")
{
    // What an older node's record looks like on the wire: the same five fields, and
    // no sixth. The nested record is read with the variable-arity split precisely so
    // this is a node reporting less, not a node speaking a shape we refuse.
    auto const cores = WireFields::ToBigEndian<std::uint32_t>(12);
    auto const memory = WireFields::ToBigEndian<std::uint64_t>(1ULL << 34);
    auto const nodeClass = std::array { static_cast<std::byte>(0) };
    auto const cache = Wire::EncodeCacheCapacity(Wire::CacheCapacityFields {});
    auto const older = WireFields::Encode({ std::span<std::byte const> { cores },
                                            std::span<std::byte const> { memory },
                                            std::span<std::byte const> { nodeClass },
                                            std::span<std::byte const> {},
                                            std::span<std::byte const> { cache } });

    auto const back = Wire::DecodeCapacity(older);
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).logicalCores == 12);
    // Empty, and that is the fact: this node cannot report a version. Somewhere
    // downstream it renders as an absence rather than as a blank, so the one machine
    // too old to answer is the one that stands out during an upgrade.
    CHECK(Unwrap(back).version.empty());
}

TEST_CASE("A version this build cannot parse is reported rather than refused", "[distributed][scheduler][protocol][version]")
{
    // A version is a string an operator reads, not one this code branches on, so a
    // shape it does not recognise is a peer to report -- refusing the registration
    // would take a working machine out of the fleet over a diagnostic field.
    auto wire = CapacityToWire(NodeCapacity { .logicalCores = 4 });
    wire.version = "not-a-version-at-all <&\"";

    // **Decoded from a temporary, deliberately.** This is the obvious spelling, and
    // it was a use-after-free while `CapacityFields::version` was a `string_view`:
    // the encoded buffer dies at the semicolon and the view outlives it. Linux never
    // noticed -- nothing reused the block before the read -- and macOS failed, on the
    // one standard library whose allocator is quick enough. The field owns its bytes
    // now, and this shape is what proves it stays that way.
    auto const back = Wire::DecodeCapacity(Wire::EncodeCapacity(wire));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).version == "not-a-version-at-all <&\"");
}

TEST_CASE("A decoded capacity record outlives the buffer it came from", "[distributed][scheduler][protocol][version]")
{
    // The rule stated on its own, rather than inferred from a case that happens to
    // exercise it: `DecodeCapacity` returns by value and nothing in the name says
    // "view", so what it returns must not point into the bytes it was handed. The
    // buffer here is scoped to prove it, and every field is read after it is gone.
    std::optional<Wire::CapacityFields> decoded;
    {
        Wire::CapacityFields wire {};
        wire.logicalCores = 12;
        wire.version = "9.9.9-rc1";
        wire.reservedMemoryBytes = 4096;
        auto const encoded = Wire::EncodeCapacity(wire);
        decoded = Wire::DecodeCapacity(encoded);
    }
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).version == "9.9.9-rc1");
    CHECK(Unwrap(decoded).logicalCores == 12U);
    CHECK(Unwrap(decoded).reservedMemoryBytes == 4096U);
}

TEST_CASE("Memory a node holds back survives the wire", "[distributed][scheduler][protocol][memory]")
{
    constexpr NodeCapacity original { .logicalCores = 64,
                                      .totalMemoryBytes = 34359738368,
                                      .reservedMemoryBytes = 8589934592,
                                      .nodeClass = NodeClass::Dedicated };

    auto const back = CapacityFromWire(CapacityToWire(original));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).reservedMemoryBytes == original.reservedMemoryBytes);
    // It travels because slot derivation can happen at either end: a node normally
    // sizes itself, but `slots = 0` asks the scheduler to -- and a scheduler
    // budgeting jobs against RAM the node already spent on its own cache would
    // over-commit exactly the machines that bothered to report it.
    CHECK(OfferableSlots(Unwrap(back), 0) == OfferableSlots(original, 0));
}

TEST_CASE("A peer too old to report held-back memory schedules as it always did",
          "[distributed][scheduler][protocol][memory]")
{
    // Six fields, no seventh -- what an older node's record looks like. It arrives
    // as zero, which is "holds nothing back", which is the arithmetic every node had
    // before this field existed.
    auto wire = CapacityToWire(NodeCapacity { .logicalCores = 64, .totalMemoryBytes = 34359738368 });
    wire.reservedMemoryBytes = 0;

    auto const back = Wire::DecodeCapacity(Wire::EncodeCapacity(wire));
    REQUIRE(back.has_value());
    CHECK(Unwrap(back).reservedMemoryBytes == 0U);
}
