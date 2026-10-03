// SPDX-License-Identifier: Apache-2.0
#include "Responders.hpp"
#include "SessionResponder.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/WireFields.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <TicketCredentials.hpp>
#include <core/async/SyncRun.hpp>
#include <core/platform/Clock.hpp>
#include <tests/LeaseRosterFakes.hpp>
#include <tests/LocalityFakes.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>
#include <tests/WireReply.hpp>

using namespace FastCache;
using namespace FastCache::Node;
using FastCache::Testing::Unwrap;
namespace Wire = FastCache::CompileCacheWire;

namespace
{

constexpr auto Noon = std::chrono::system_clock::time_point { std::chrono::hours { 500'000 } };

/// The one endpoint this node answers to as a ticket audience.
constexpr std::string_view OfficeEndpoint = "office.corp:6674";

/// Matches `OfficeEndpoint` alone: what these cases ask is what the session surface does with
/// the audience's answer, not how `NodeAudience` computes it.
class FixedAudience final: public Distributed::IAudience
{
  public:
    [[nodiscard]] bool Matches(std::string_view audience) const override
    {
        return audience == OfficeEndpoint;
    }
};

/// One node's session surface: machine `office` minting with its own key, verifying against a
/// roster that admits `office` and `pc-07`.
struct Session
{
    core::platform::ManualWallClock wallClock { Noon };
    AtomicMetricsSink metrics;
    Testing::FixedLeaseRoster roster { { "office" } };
    FixedAudience audience;
    Distributed::SpentTickets spent;
    Distributed::TicketVerifier verifier { &roster, audience, spent };
    Ed25519KeyPair key = Testing::TestKeyPair("office");
    Testing::ScriptedSecureRandom random { Testing::ScriptedSecureRandom::Ascending(64) };
    SessionResponder session {
        verifier, SessionKeys { .identityKey = &key, .machineId = "office" }, random, wallClock, metrics
    };

    Session()
    {
        roster.AdmitMachine("pc-07");
    }

    /// Ask this node for a ticket.
    /// @param requested The endpoint the ticket is for.
    /// @param host Who asks.
    /// @return The reply.
    [[nodiscard]] FrameReply Mint(std::string_view requested, std::string host = "127.0.0.1")
    {
        return core::async::syncRun(
            session.Answer(Wire::EncodeMintTicketRequest(requested), PeerIdentity { .host = std::move(host) }));
    }
};

/// @param ticket A machine ticket's bytes.
/// @return The payload of an `AUTH` presenting it.
[[nodiscard]] std::vector<std::byte> TicketAuth(std::span<std::byte const> ticket)
{
    auto const frame = Wire::EncodeAuth(
        Wire::AuthRequest { .kind = Wire::AuthKind::MachineTicket, .username = {}, .secret = AsStringView(ticket) });
    return { frame.begin() + static_cast<std::ptrdiff_t>(Wire::RequestHeaderSize), frame.end() };
}

/// @param reply An `Ok` reply; REQUIREs the status.
/// @return Its body.
[[nodiscard]] std::vector<std::byte> OkPayloadOf(std::span<std::byte const> reply)
{
    REQUIRE(Testing::StatusOf(reply) == std::optional { Wire::Status::Ok });
    auto const body = Testing::PayloadOf(reply);
    return { body.begin(), body.end() };
}

/// @param at An instant.
/// @return It in Unix seconds.
[[nodiscard]] std::uint64_t UnixSeconds(std::chrono::system_clock::time_point at)
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count());
}

} // namespace

TEST_CASE("AUTH reaches the session component from any address, before membership is asked", "[node][session]")
{
    Session node;
    CHECK_FALSE(
        node.session.RefusePeer(PeerIdentity { .host = "10.0.0.7" }, static_cast<std::uint8_t>(Wire::Op::Auth)).has_value());
}

TEST_CASE("A node with no password answers AUTH Ok and establishes nothing", "[node][session]")
{
    Session node;
    auto const frame = Wire::EncodeAuth(Wire::AuthRequest { .username = {}, .secret = "whatever" });
    auto const verdict = node.session.CheckCredential(std::span { frame }.subspan(Wire::RequestHeaderSize));
    CHECK(verdict.outcome == CredentialOutcome::NoPolicy);
    CHECK_FALSE(verdict.machine.has_value());
    CHECK(verdict.refusalReply.empty());

    // The control that makes the answer above about the PASSWORD rather than about any payload at
    // all: one that will not decode is told so, so a client of another build is not answered Ok.
    std::vector<std::byte> const garbage { std::byte { 0x7F } };
    CHECK(node.session.CheckCredential(garbage).outcome == CredentialOutcome::Malformed);

    // And neither is a ticket event.
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 0);
}

TEST_CASE("The Session family is answered on every built node", "[node][session][surface]")
{
    CHECK(FamilyRoutes[static_cast<std::size_t>(Wire::VerbFamily::Session)].presence == FamilyPresence::OnEveryBuiltNode);
    CHECK(FamilyRoutes[static_cast<std::size_t>(Wire::VerbFamily::Session)].owner == &SurfaceComponents::session);
}

namespace
{

/// The launcher's side of a mint, answered by a real session surface as this machine's node: each
/// frame goes to `Answer` from loopback, and the reply comes back as the outcome the launcher reads.
class NodeOnThisMachine final: public Cc::IEndpointExchange
{
  public:
    explicit NodeOnThisMachine(SessionResponder& session):
        _session { session }
    {
    }

    [[nodiscard]] Cc::CacheOutcome Exchange(std::string_view /*hostPort*/,
                                            std::vector<std::byte> frame,
                                            Cc::Credential const& /*credential*/,
                                            Cc::ExchangeBudget /*budget*/) override
    {
        auto const reply = core::async::syncRun(_session.Answer(frame, PeerIdentity { .host = "127.0.0.1" })).bytes;
        auto outcome = Cc::CacheOutcome {};
        if (Testing::StatusOf(reply) == std::optional { Wire::Status::Ok })
        {
            auto const body = Testing::PayloadOf(reply);
            outcome.kind = Cc::CacheOutcomeKind::Hit;
            outcome.value.assign(body.begin(), body.end());
            return outcome;
        }
        outcome.kind = Cc::CacheOutcomeKind::Rejected;
        outcome.code = Testing::ErrorOf(reply).value_or(Wire::ErrorCode::MalformedFrame);
        return outcome;
    }

  private:
    SessionResponder& _session;
};

} // namespace

TEST_CASE("A launcher asks this node for no ticket it would refuse, so the refusal counter never moves",
          "[node][session][ticket]")
{
    // The launcher's choice and the minter's refusal are ONE rule (`NamesNoOneMachine`), asked at
    // both ends: `FASTCACHE_ADDR=localhost:6674` used to ask for a ticket naming `localhost` on
    // every FETCH and STORE, which this node refused and counted each time.
    Session node;
    NodeOnThisMachine exchange { node.session };
    auto said = std::vector<std::string> {};
    Cc::TicketCredentials tickets { exchange,
                                    std::string { "127.0.0.1:6674" },
                                    Cc::Credential {},
                                    std::string {},
                                    Cc::ExchangeBudget {},
                                    [&said](std::string_view line) { said.emplace_back(line); } };

    for (auto const* everybody: { "localhost:6674", "LOCALHOST:6674", "127.0.0.1:6674", "0.0.0.0:6674", "[::]:6674" })
    {
        INFO(everybody);
        CHECK_FALSE(tickets.For(everybody).Configured());
    }
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedMalformed) == 0);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 0);
    CHECK(said.empty());

    // The control: an audience naming one machine is minted, through the same surface.
    auto const office = tickets.For(OfficeEndpoint);
    CHECK(office.kind == Wire::AuthKind::MachineTicket);
    CHECK(office.Configured());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedMalformed) == 0);
}

TEST_CASE("A process on this machine is minted a ticket this node's own roster accepts", "[node][session][ticket]")
{
    Session node;
    auto const reply = node.Mint(OfficeEndpoint);
    auto const ticket = OkPayloadOf(reply.bytes);
    auto const verdict = node.session.CheckCredential(TicketAuth(ticket));
    CHECK(verdict.outcome == CredentialOutcome::Accepted);
    REQUIRE(verdict.machine.has_value());
    CHECK(Unwrap(verdict.machine).id == "office");
    CHECK(Unwrap(verdict.machine).key == node.key.PublicKey());
    CHECK(verdict.refusalReply.empty());
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 1);

    // Its claims are the ones stated: this node's id, the audience asked for, one lifetime from
    // now, and the nonce the random source drew.
    auto const decoded = Distributed::DecodeMachineTicket(ticket);
    REQUIRE(decoded.has_value());
    CHECK(Unwrap(decoded).claims.machineId == "office");
    CHECK(Unwrap(decoded).claims.audience == OfficeEndpoint);
    CHECK(Unwrap(decoded).claims.expiresAtUnixSeconds == UnixSeconds(Noon + Distributed::MachineTicketLifetime));
    CHECK(std::ranges::equal(Unwrap(decoded).claims.nonce,
                             Testing::ScriptedSecureRandom::Ascending(Distributed::MachineTicketNonceBytes)));
}

TEST_CASE("A ticket is minted for this machine's processes only, and never for an audience every node shares",
          "[node][session][ticket]")
{
    // A ticket is this machine vouching for its caller, so the one caller it must never vouch
    // for is on another machine. Judged from THIS connection's peer address and nothing cached:
    // loopback is admitted at the door, anything else is refused by name and counted -- and
    // `Answer`, which is reachable directly, asks again rather than taking the door's word.
    Session node;
    auto const opRaw = static_cast<std::uint8_t>(Wire::Op::MintTicket);
    for (auto const* host: { "127.0.0.1", "127.0.0.2", "::1", "::ffff:127.0.0.1" })
    {
        INFO(host);
        CHECK_FALSE(node.session.RefusePeer(PeerIdentity { .host = host }, opRaw).has_value());
    }

    // What the cache tier's interval-refreshed oracle would call this machine, one address of which
    // (10.0.0.9) this machine has since LOST -- the stale answer that fails open for FETCH. Minting
    // does not ask it, so neither a remote peer, nor a peer on another of this host's addresses, nor
    // the stale address is vouched for: loopback is the whole rule.
    Testing::ThisMachineIs const cacheTierSays { "127.0.0.1", "10.0.0.5", "10.0.0.9" };
    for (auto const* host: { "10.0.0.9", "10.0.0.5", "10.0.0.7", "" })
    {
        INFO(host);
        INFO("the cache tier's oracle says this machine: " << cacheTierSays.IsThisMachine(host));
        auto const atTheDoor = node.session.RefusePeer(PeerIdentity { .host = host }, opRaw);
        REQUIRE(atTheDoor.has_value());
        CHECK(Testing::ErrorOf(Unwrap(atTheDoor)) == Wire::ErrorCode::NotAMember);
    }
    CHECK(Testing::ErrorOf(node.Mint(OfficeEndpoint, "10.0.0.9").bytes) == Wire::ErrorCode::NotAMember);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNotLocal) == 5);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 0);

    // The control that makes the refusals above about the ADDRESS: the same request over loopback
    // is signed.
    CHECK(Testing::StatusOf(node.Mint(OfficeEndpoint, "::1").bytes) == std::optional { Wire::Status::Ok });

    // An audience every node would answer to is refused before anything is signed.
    for (auto const* audience: { "127.0.0.1:6674", "localhost:6674", "[::1]:6674", "0.0.0.0:6674", "6674", "" })
    {
        INFO(audience);
        CHECK(Testing::ErrorOf(node.Mint(audience).bytes) == Wire::ErrorCode::MalformedFrame);
    }
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedMalformed) == 6);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 1);
}

TEST_CASE("A node that cannot draw a nonce, or holds no key, mints nothing and says which", "[node][session][ticket]")
{
    Session node;
    Testing::ScriptedSecureRandom failing { Testing::ScriptedSecureRandom::DeniedFailure() };
    SessionResponder noRandom {
        node.verifier, SessionKeys { .identityKey = &node.key, .machineId = "office" }, failing, node.wallClock, node.metrics
    };
    auto const refused = core::async::syncRun(
        noRandom.Answer(Wire::EncodeMintTicketRequest(OfficeEndpoint), PeerIdentity { .host = "127.0.0.1" }));
    CHECK(Testing::ErrorOf(refused.bytes) == Wire::ErrorCode::NoCluster);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNoRandom) == 1);

    SessionResponder keyless { node.verifier, SessionKeys {}, node.random, node.wallClock, node.metrics };
    auto const none = core::async::syncRun(
        keyless.Answer(Wire::EncodeMintTicketRequest(OfficeEndpoint), PeerIdentity { .host = "127.0.0.1" }));
    CHECK(Testing::ErrorOf(none.bytes) == Wire::ErrorCode::NoCluster);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNoKey) == 1);

    // Each refusal moved its own row alone.
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedNoRandom) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 0);
}

TEST_CASE("A refused ticket is answered by its reason and establishes nothing", "[node][session][ticket]")
{
    // Claims that are good in every respect but the signature, so it fails on that and nothing else.
    Session node;
    auto const forged = Distributed::MintMachineTicket(
        Testing::TestKeyPair("impostor"),
        Distributed::MachineTicketClaims { .machineId = "pc-07",
                                           .audience = std::string { OfficeEndpoint },
                                           .expiresAtUnixSeconds = UnixSeconds(Noon + std::chrono::seconds { 30 }),
                                           .nonce = {} });
    auto const verdict = node.session.CheckCredential(TicketAuth(AsBytes(forged.View())));
    CHECK(verdict.outcome == CredentialOutcome::Rejected);
    CHECK_FALSE(verdict.machine.has_value());
    CHECK(Testing::ErrorOf(verdict.refusalReply) == Wire::ErrorCode::TicketRefused);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedForged) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsAccepted) == 0);

    // The control: the same claims under pc-07's own key are accepted, so the refusal above is
    // about the signature rather than about anything else these claims say.
    auto const genuine = Distributed::MintMachineTicket(
        Testing::TestKeyPair("pc-07"),
        Distributed::MachineTicketClaims { .machineId = "pc-07",
                                           .audience = std::string { OfficeEndpoint },
                                           .expiresAtUnixSeconds = UnixSeconds(Noon + std::chrono::seconds { 30 }),
                                           .nonce = {} });
    auto const accepted = node.session.CheckCredential(TicketAuth(AsBytes(genuine.View())));
    CHECK(accepted.outcome == CredentialOutcome::Accepted);
    REQUIRE(accepted.machine.has_value());
    CHECK(Unwrap(accepted.machine).id == "pc-07");
}

TEST_CASE("A ticket refused for a revoked key admits nothing and names the forgotten machine",
          "[node][session][ticket][forget]")
{
    // A refused ticket grants no admission -- and the one refused because its key was REVOKED is
    // evidence that the presenter is the machine a forget removed, which the endpoint keeps so the
    // verbs behind it are refused as that machine's.
    Session node;
    node.roster.Revoke("pc-07");
    auto const ticket = Distributed::MintMachineTicket(
        Testing::TestKeyPair("pc-07"),
        Distributed::MachineTicketClaims { .machineId = "pc-07",
                                           .audience = std::string { OfficeEndpoint },
                                           .expiresAtUnixSeconds = UnixSeconds(Noon + std::chrono::seconds { 30 }),
                                           .nonce = {} });
    auto const verdict = node.session.CheckCredential(TicketAuth(AsBytes(ticket.View())));
    CHECK(verdict.outcome == CredentialOutcome::Rejected);
    CHECK_FALSE(verdict.machine.has_value());
    REQUIRE(verdict.revokedMachine.has_value());
    CHECK(Unwrap(verdict.revokedMachine).Id() == "pc-07");
    CHECK(Testing::ErrorOf(verdict.refusalReply) == Wire::ErrorCode::TicketRefused);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsRefusedRevoked) == 1);

    // The control: a refusal for any other reason carries no such evidence -- here a forgery of an
    // admitted machine, which says nothing about who the presenter is.
    auto const forged = Distributed::MintMachineTicket(
        Testing::TestKeyPair("impostor"),
        Distributed::MachineTicketClaims { .machineId = "office",
                                           .audience = std::string { OfficeEndpoint },
                                           .expiresAtUnixSeconds = UnixSeconds(Noon + std::chrono::seconds { 30 }),
                                           .nonce = {} });
    auto const refused = node.session.CheckCredential(TicketAuth(AsBytes(forged.View())));
    CHECK(refused.outcome == CredentialOutcome::Rejected);
    CHECK_FALSE(refused.revokedMachine.has_value());
}

TEST_CASE("An audience that is not text is refused before anything is signed, and not echoed", "[node][session][ticket]")
{
    // Every verifier refuses a ticket whose audience is not UTF-8 (`not-utf8`), so signing one would
    // hand the caller a credential no node accepts. The minter asks the verifier's own check first,
    // and its refusal must not carry the bytes back: a refusal message is text as well.
    Session node;
    auto const refused = node.Mint("office.corp\xff:6674");
    CHECK(Testing::ErrorOf(refused.bytes) == Wire::ErrorCode::MalformedFrame);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketMintsRefusedMalformed) == 1);
    CHECK(node.metrics.Read(IMetricsSink::Counter::NodeTicketsMinted) == 0);
    auto const payload = Testing::PayloadOf(refused.bytes);
    CHECK(std::ranges::find(payload, std::byte { 0xFF }) == payload.end());

    // The control: the same audience, as text, is signed.
    CHECK(Testing::StatusOf(node.Mint(OfficeEndpoint).bytes) == std::optional { Wire::Status::Ok });
}
