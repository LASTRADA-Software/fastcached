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

TEST_CASE("A scheduler answers its three verbs and nothing else", "[distributed][scheduler][protocol]")
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
