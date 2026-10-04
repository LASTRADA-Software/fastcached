// SPDX-License-Identifier: Apache-2.0
#include "EndpointDialerTestUtils.hpp"
#include "FleetProbe.hpp"

#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <core/net/testing/SocketDecorator.hpp>
#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/ScriptedSocket.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using CompileCacheWire::FleetState;
using CompileCacheWire::FleetSummary;
using FastCache::Testing::Unwrap;

namespace Wire = FastCache::CompileCacheWire;

namespace
{
/// The nonces a scripted generator serves: `Ascending`, so the first question's is 0, 1, ... 31.
/// @return The script.
[[nodiscard]] std::vector<std::byte> NonceScript()
{
    return Testing::ScriptedSecureRandom::Ascending(NonceBytes);
}

/// The office seed's summary.
/// @return The summary.
[[nodiscard]] FleetSummary Office()
{
    return FleetSummary { .clusterId = "c-office",
                          .state = FleetState::Established,
                          .leaderNodeEndpoint = "office.example:6674",
                          .nodeId = "n-office",
                          .raftEndpoint = "office.example:6680" };
}

/// The framed `Ok` reply a seed gives, signed by `n-office` over @p nonce.
/// @param nonce The nonce it signs over.
/// @return The reply frame.
[[nodiscard]] std::vector<std::byte> SignedOver(std::span<std::byte const> nonce)
{
    auto const key = Testing::TestKeyPair("n-office");
    auto reply = Wire::FleetSummaryReply { .summary = Office() };
    reply.publicKey = key.PublicKey();
    reply.signature = SignLabelled(key, Cluster::FleetSummaryMessage(nonce, reply.summary, reply.publicKey));
    return Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeFleetSummaryReply(reply));
}
} // namespace

TEST_CASE("A probe asks a seed with a fresh nonce and holds only what its signature proves", "[node][formation][summary]")
{
    // The nonce this probe will draw is scripted, so the seed's answer can be signed over it: the
    // same bytes a live seed would sign.
    auto const script = NonceScript();
    Testing::ScriptedDialer dialer { { SignedOver(script) } };
    core::platform::ManualClock clock;
    Testing::ScriptedSecureRandom random { script };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::DnsSrv });
    REQUIRE(fleet.has_value());
    CHECK(fleet->Summary() == Office());
    CHECK(fleet->Key() == Testing::TestKeyPair("n-office").PublicKey());
    CHECK(fleet->Origin() == Cluster::FleetOrigin::DnsSrv);

    // It asked the endpoint it was given, with exactly the nonce it drew.
    REQUIRE(dialer.Dialed() == std::vector<std::string> { "office.example:6674" });
    auto const sent = dialer.SentOn(0);
    auto const header = Wire::DecodeRequestHeader(sent);
    REQUIRE(header.has_value());
    CHECK(Unwrap(header).opRaw == static_cast<std::uint8_t>(Wire::Op::FleetSummary));
    auto const asked = Wire::DecodeFleetSummaryRequestPayload(sent.subspan(Wire::RequestHeaderSize));
    REQUIRE(asked.has_value());
    CHECK(std::ranges::equal(Unwrap(asked), script));
}

TEST_CASE("A summary question returns the list a reply carries, proven and claiming no origin",
          "[node][formation][summary][split]")
{
    // The question a pointer is followed and a leader's key is vouched with: the same exchange as a
    // seed's, its answer held to the same signature over the same fresh nonce -- and a list at the one
    // member cap every carrier holds, with a total past it.
    auto const script = NonceScript();
    auto summary = Office();
    for (auto const index: std::views::iota(std::size_t { 0 }, Wire::MaxFleetSummaryMembers))
        summary.members.push_back(std::format("n-{}", index));
    summary.memberTotal = summary.members.size() + 9;
    auto const key = Testing::TestKeyPair("n-office");
    auto reply = Wire::FleetSummaryReply { .summary = summary };
    reply.publicKey = key.PublicKey();
    reply.signature = SignLabelled(key, Cluster::FleetSummaryMessage(script, reply.summary, reply.publicKey));
    Testing::ScriptedDialer dialer { { Wire::EncodeReply(Wire::Status::Ok, Wire::EncodeFleetSummaryReply(reply)) } };
    core::platform::ManualClock clock;
    Testing::ScriptedSecureRandom random { script };
    DialledFleetProbe probe { dialer, random, clock };

    auto const proven = probe.AskSummary("office.example:6674");
    REQUIRE(proven.has_value());
    CHECK(proven->Summary() == summary);
    CHECK(proven->Key() == key.PublicKey());
    CHECK(dialer.Dialed() == std::vector<std::string> { "office.example:6674" });

    // A replayed answer is refused here as a seed's is.
    auto recorded = NonceScript();
    recorded[0] = std::byte { 0xEE };
    Testing::ScriptedDialer replaying { { SignedOver(recorded) } };
    Testing::ScriptedSecureRandom again { NonceScript() };
    DialledFleetProbe second { replaying, again, clock };
    auto const refused = second.AskSummary("office.example:6674");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().contains("not signed over this question"));
}

TEST_CASE("A probe refuses an answer signed over another question, naming the seed", "[node][formation][summary]")
{
    // A recorded answer, replayed: signed over a nonce this probe never drew.
    auto recorded = NonceScript();
    recorded[0] = std::byte { 0xEE };
    Testing::ScriptedDialer dialer { { SignedOver(recorded) } };
    core::platform::ManualClock clock;
    Testing::ScriptedSecureRandom random { NonceScript() };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::Remembered });
    REQUIRE_FALSE(fleet.has_value());
    CHECK(fleet.error().contains("office.example:6674"));
    CHECK(fleet.error().contains("not signed over this question"));
}

TEST_CASE("A probe says which seed it could not reach, refused, or could not ask", "[node][formation][summary]")
{
    SECTION("unreachable")
    {
        Testing::ScriptedDialer dialer { { {} } };
        core::platform::ManualClock clock;
        Testing::ScriptedSecureRandom random { NonceScript() };
        DialledFleetProbe probe { dialer, random, clock };
        auto const fleet = probe.Ask({ .endpoint = "gone.example:6674", .source = Cluster::SeedSource::FleetSeedFlag });
        REQUIRE_FALSE(fleet.has_value());
        CHECK(fleet.error().contains("cannot reach gone.example:6674"));
    }
    SECTION("refused")
    {
        Testing::ScriptedDialer dialer { { Wire::EncodeErrorReply(Wire::ErrorCode::NoCluster, "no fleet here") } };
        core::platform::ManualClock clock;
        Testing::ScriptedSecureRandom random { NonceScript() };
        DialledFleetProbe probe { dialer, random, clock };
        auto const fleet = probe.Ask({ .endpoint = "worker.example:6674", .source = Cluster::SeedSource::FleetSeedFlag });
        REQUIRE_FALSE(fleet.has_value());
        CHECK(fleet.error().contains("worker.example:6674"));
        CHECK(fleet.error().contains("no fleet here"));
    }
    SECTION("this host cannot draw a nonce, and nothing is dialled")
    {
        Testing::ScriptedDialer dialer { {} };
        core::platform::ManualClock clock;
        Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::DeniedFailure() };
        DialledFleetProbe probe { dialer, random, clock };
        auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::FleetSeedFlag });
        REQUIRE_FALSE(fleet.has_value());
        CHECK(fleet.error().contains("this node cannot draw a nonce"));
        CHECK(dialer.Dialed().empty());
    }
}

namespace
{
/// A seed that answers from a script, paced on the probe's own clock, and says how large a read the
/// probe asked it for.
///
/// Both halves are what the reviewer's probes measured on a real machine -- wall time and peak
/// working set -- made deterministic: each read costs `perRead` of the injected clock, and the
/// largest buffer a read was handed is exactly the allocation a declared length provoked.
class PacedSeed final: public core::net::testing::SocketDecorator
{
  public:
    /// @param scripted What the seed sends; must outlive this.
    /// @param clock The probe's clock, advanced by every read.
    /// @param perRead What each read costs on it.
    /// @param bytesPerRead The most one read hands over; one is a dribble.
    PacedSeed(Testing::ScriptedSocket& scripted,
              core::platform::ManualClock& clock,
              std::chrono::milliseconds perRead,
              std::size_t bytesPerRead):
        SocketDecorator { scripted },
        _clock { clock },
        _perRead { perRead },
        _bytesPerRead { bytesPerRead }
    {
    }

    [[nodiscard]] core::net::IoAwaitable read(std::span<std::byte> buffer) override
    {
        _largestRead = std::max(_largestRead, buffer.size());
        _clock.advance(_perRead);
        return inner().read(buffer.first(std::min(buffer.size(), _bytesPerRead)));
    }

    /// @return The largest buffer the reader handed any read.
    [[nodiscard]] std::size_t LargestRead() const noexcept
    {
        return _largestRead;
    }

  private:
    core::platform::ManualClock& _clock;
    std::chrono::milliseconds _perRead;
    std::size_t _bytesPerRead;
    std::size_t _largestRead { 0 };
};

/// Dials one `PacedSeed`, and keeps it readable after the probe is done with the connection.
class PacedDialer final: public IEndpointDialer
{
  public:
    /// @param seed The one seed every dial reaches; must outlive this.
    explicit PacedDialer(PacedSeed& seed):
        _seed { seed }
    {
    }

    [[nodiscard]] std::unique_ptr<core::net::ISocket> Dial(std::string_view /*endpoint*/,
                                                           core::net::DialOptions /*options*/) override
    {
        return std::make_unique<core::net::testing::SocketDecorator>(static_cast<core::net::ISocket&>(_seed));
    }

  private:
    PacedSeed& _seed;
};
} // namespace

TEST_CASE("A seed that pulses on a verb that never pulses is refused, not waited on", "[node][formation][summary][security]")
{
    // The reviewer's first probe, kept: 200000 `Progress` frames, then the real answer. FLEET-SUMMARY
    // does not pulse -- its `legalStatuses` says so -- so the first pulse ends the exchange, and the
    // seed's bytes after it are never read.
    auto const script = NonceScript();
    auto frames = std::vector<std::byte> {};
    auto const pulse = Wire::EncodeReply(Wire::Status::Progress, {});
    for ([[maybe_unused]] auto const index: std::views::iota(0, 200000))
        frames.insert(frames.end(), pulse.begin(), pulse.end());
    auto const answer = SignedOver(script);
    frames.insert(frames.end(), answer.begin(), answer.end());

    core::platform::ManualClock clock;
    Testing::ScriptedSocket scripted { frames };
    PacedSeed seed { scripted, clock, std::chrono::milliseconds { 0 }, frames.size() };
    PacedDialer dialer { seed };
    Testing::ScriptedSecureRandom random { script };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::DnsSrv });
    REQUIRE_FALSE(fleet.has_value());
    CHECK(fleet.error().contains("office.example:6674"));
}

TEST_CASE("A seed's declared reply length is refused before anything is allocated for it",
          "[node][formation][summary][security]")
{
    // The reviewer's second probe, kept: an `Ok` header declaring 512 MiB, then nothing. The reply
    // ceiling is read off the verb's row BEFORE the payload is read, so no read is ever handed a
    // buffer the size of what the stranger declared -- only the five-byte header was asked for.
    auto const declared = std::vector<std::byte> { std::byte { static_cast<std::uint8_t>(Wire::Status::Ok) },
                                                   std::byte { 0x20 },
                                                   std::byte { 0x00 },
                                                   std::byte { 0x00 },
                                                   std::byte { 0x00 } };
    REQUIRE(declared.size() == Wire::ReplyHeaderSize);

    core::platform::ManualClock clock;
    Testing::ScriptedSocket scripted { declared };
    PacedSeed seed { scripted, clock, std::chrono::milliseconds { 0 }, declared.size() };
    PacedDialer dialer { seed };
    Testing::ScriptedSecureRandom random { NonceScript() };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::DnsSrv });
    REQUIRE_FALSE(fleet.has_value());
    CHECK(seed.LargestRead() <= Wire::ReplyHeaderSize);
}

TEST_CASE("A seed that dribbles its answer is abandoned at one deadline over the whole question",
          "[node][formation][summary][security]")
{
    // A per-call bound holds a reader forever against a peer that sends one byte just inside it. The
    // honest answer below arrives in full, one byte per read -- and each read costs a second of the
    // probe's own clock, so the question's one deadline runs out long before the answer does.
    auto const script = NonceScript();
    auto const answer = SignedOver(script);
    REQUIRE(answer.size() > static_cast<std::size_t>(DialledFleetProbe::ExchangeDeadline / std::chrono::seconds { 1 }));

    core::platform::ManualClock clock;
    Testing::ScriptedSocket scripted { answer };
    PacedSeed seed { scripted, clock, std::chrono::seconds { 1 }, 1 };
    PacedDialer dialer { seed };
    Testing::ScriptedSecureRandom random { script };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::DnsSrv });
    REQUIRE_FALSE(fleet.has_value());
    CHECK(fleet.error().contains("did not answer which fleet it is in within"));

    // The control: the same dribble on a clock that does not move is answered in full, so what
    // refused it above is the deadline and not the pacing.
    core::platform::ManualClock still;
    Testing::ScriptedSocket patientScript { answer };
    PacedSeed patient { patientScript, still, std::chrono::milliseconds { 0 }, 1 };
    PacedDialer patientDialer { patient };
    Testing::ScriptedSecureRandom again { script };
    DialledFleetProbe patientProbe { patientDialer, again, still };
    CHECK(patientProbe.Ask({ .endpoint = "office.example:6674", .source = Cluster::SeedSource::DnsSrv }).has_value());
}

TEST_CASE("A seed's refusal sentence reaches the probe's error only as bounded text", "[node][formation][summary][security]")
{
    // The sentence is the seed's, unauthenticated, of any length and any bytes, and the error goes
    // into log lines: it is escaped and cut.
    auto const sentence = std::string(10'000, 'x') + "\xFF";
    auto const refusal = Wire::EncodeErrorReply(Wire::ErrorCode::NoCluster, "\xFF" + sentence);
    core::platform::ManualClock clock;
    Testing::ScriptedDialer dialer { { refusal } };
    Testing::ScriptedSecureRandom random { NonceScript() };
    DialledFleetProbe probe { dialer, random, clock };

    auto const fleet = probe.Ask({ .endpoint = "worker.example:6674", .source = Cluster::SeedSource::FleetSeedFlag });
    REQUIRE_FALSE(fleet.has_value());
    CHECK(fleet.error().contains("\\xFF"));
    CHECK_FALSE(fleet.error().contains("\xFF"));
    CHECK(fleet.error().size() < DialledFleetProbe::MaxRefusalText + 128);
}
