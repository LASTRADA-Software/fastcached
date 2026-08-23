// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/WireFrame.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
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

TEST_CASE("A whole register-heartbeat-lease exchange crosses the wire", "[distributed][scheduler][protocol]")
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
