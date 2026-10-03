// SPDX-License-Identifier: Apache-2.0
#include "SessionResponder.hpp"

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/SecureBytes.hpp>
#include <FastCache/Distributed/MachineTicket.hpp>
#include <FastCache/Protocol/CompileCacheAuth.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>
#include <utility>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// Why a size or framing refusal on this surface moves nothing, stated once for the arms that
    /// share it.
    ///
    /// The `Session` surface counts ticket refusals BY REASON (`TicketRefusalTable`), each in
    /// the reply it encodes itself, and its mint refusals by their own rows; a size or framing
    /// refusal says the peer is confused, and summed into either it would bury the refusals that
    /// mean a ticket is wrong somewhere.
    constexpr std::string_view SessionRefusalRationale =
        "the Session surface counts ticket refusals by reason (TicketRefusalTable); a size refusal says the peer is "
        "confused";

    /// Why refusing a verb this node does not serve moves nothing.
    constexpr std::string_view NotServedRationale =
        "a healthy answer, not an event: a client steps over a verb this node does not serve";

    /// This surface's refusal rows: none today, since it serves both `Session` verbs.
    ///
    /// The shape, the lookup and why they exist are on `Wire::RefusedVerb`. Kept as an empty table
    /// rather than deleted so the next verb this family gains and this node does not serve is a
    /// row, not a `case`.
    constexpr std::array<Wire::RefusedVerb, 0> RefusedVerbs {};

    // A row naming a verb this surface serves would sit there looking like a decision and change
    // nothing: the lookup is reached only by a verb it does not serve.
    static_assert(std::ranges::none_of(
                      RefusedVerbs,
                      [](Wire::Op op) { return op == Wire::Op::Auth || op == Wire::Op::MintTicket; },
                      &Wire::RefusedVerb::op),
                  "a refusal row for a verb this surface serves is dead: the lookup never reaches it");

    /// The refusal a `Session` verb this node does not serve is answered with.
    ///
    /// Its row when it has one, and the generic `UnimplementedVerb` otherwise: a verb this build
    /// adds to the family is refused by name rather than served by accident.
    /// @param opRaw The third header byte, as received.
    /// @return The encoded refusal.
    [[nodiscard]] std::vector<std::byte> RefuseVerb(std::uint8_t opRaw)
    {
        auto const* const descriptor = Wire::FindOp(opRaw);
        auto const* const row = descriptor == nullptr ? nullptr : Wire::FindRefusal(RefusedVerbs, descriptor->code);
        if (row != nullptr)
            return Cc::RefuseWithoutCounter({ .code = row->code, .rationale = NotServedRationale }, row->why);
        return Cc::RefuseWithoutCounter({ .code = Wire::UnimplementedVerb, .rationale = NotServedRationale },
                                        "this node serves no component for that verb");
    }

    /// A `MINT-TICKET` from anywhere but loopback.
    constexpr Cc::SurfaceRefusal MintNotLocal { .code = Wire::ErrorCode::NotAMember,
                                                .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNotLocal };

    /// A `MINT-TICKET` whose audience will not decode or names no one machine.
    constexpr Cc::SurfaceRefusal MintMalformed { .code = Wire::ErrorCode::MalformedFrame,
                                                 .counter = IMetricsSink::Counter::NodeTicketMintsRefusedMalformed };

    /// A `MINT-TICKET` on a node with no identity key.
    constexpr Cc::SurfaceRefusal MintNoKey { .code = Wire::ErrorCode::NoCluster,
                                             .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNoKey };

    /// A `MINT-TICKET` this node could not draw a nonce for.
    constexpr Cc::SurfaceRefusal MintNoRandom { .code = Wire::ErrorCode::NoCluster,
                                                .counter = IMetricsSink::Counter::NodeTicketMintsRefusedNoRandom };

    /// @param at An instant.
    /// @return It in Unix seconds, as a ticket's expiry claim carries it.
    [[nodiscard]] std::uint64_t UnixSeconds(std::chrono::system_clock::time_point at) noexcept
    {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count());
    }

    /// One row per `EndpointRefusal`: why this surface counts none of them.
    struct SessionEndpointRefusal
    {
        EndpointRefusal refusal;    ///< Which endpoint decision this describes.
        std::string_view rationale; ///< Why nothing is counted.
    };

    /// What this surface does about each endpoint-decided refusal: counts none, each for its reason.
    constexpr EnumTable<EndpointRefusal, SessionEndpointRefusal> SessionEndpointRefusals { {
        { .refusal = EndpointRefusal::InFlightBudget,
          .rationale = "the byte budget says this surface is momentarily full, which the peer sees and retries" },
        { .refusal = EndpointRefusal::CredentialMalformed, .rationale = SessionRefusalRationale },
        { .refusal = EndpointRefusal::CredentialRejected, .rationale = SessionRefusalRationale },
        { .refusal = EndpointRefusal::AnswerDeadline, .rationale = AnswerDeadlineIsTheEndpointsRationale },
        { .refusal = EndpointRefusal::NodeProofUnchallenged, .rationale = NodeProofIsTheProversRationale },
    } };

    static_assert(RowsInEnumeratorOrder(SessionEndpointRefusals, &SessionEndpointRefusal::refusal),
                  "SessionEndpointRefusals must hold one row per EndpointRefusal, in enumerator order");
    static_assert(std::ranges::none_of(SessionEndpointRefusals,
                                       [](SessionEndpointRefusal const& row) { return row.rationale.empty(); }),
                  "every uncounted session endpoint refusal must say why");
} // namespace

SessionResponder::SessionResponder(Distributed::TicketVerifier const& verifier,
                                   SessionKeys keys,
                                   ISecureRandom& random,
                                   core::platform::WallClockRef wallClock,
                                   IMetricsSink& metrics) noexcept:
    _verifier { verifier },
    _identityKey { keys.identityKey },
    _machineId { keys.machineId },
    _random { random },
    _wallClock { wallClock },
    _metrics { metrics }
{
}

core::async::Task<FrameReply> SessionResponder::Answer(std::span<std::byte const> frame, PeerIdentity peer)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Empty is CLOSE, and it is the right answer to exactly this: a frame whose header will
        // not decode is not this protocol. Every other refusal is a reply.
        co_return std::vector<std::byte> {};

    if (header->opRaw == static_cast<std::uint8_t>(Wire::Op::Auth))
        // Terminated by the endpoint, which owns the connection state an `AUTH` changes; reaching
        // this means a caller went round the endpoint, which is a wiring fact and not an event.
        co_return Cc::RefuseWithoutCounter({ .code = Wire::UnimplementedVerb,
                                             .rationale = "AUTH is answered by the endpoint, which owns the connection "
                                                          "state it changes; reaching this is a wiring fact" },
                                           "AUTH is answered by the endpoint");

    if (header->opRaw == static_cast<std::uint8_t>(Wire::Op::MintTicket))
    {
        // Re-asked rather than taken on the endpoint's word: `Answer` is reachable directly, and a
        // rule enforced only at the door is one a later caller walks around.
        if (auto refusal = RefusePeer(peer, header->opRaw); refusal.has_value())
            co_return *std::move(refusal);
        co_return Mint(frame.subspan(Wire::RequestHeaderSize));
    }
    co_return RefuseVerb(header->opRaw);
}

std::optional<std::vector<std::byte>> SessionResponder::RefusePeer(PeerIdentity const& peer, std::uint8_t opRaw) const
{
    if (opRaw == static_cast<std::uint8_t>(Wire::Op::Auth))
        return std::nullopt;
    if (opRaw == static_cast<std::uint8_t>(Wire::Op::MintTicket))
    {
        // The kernel's peer address for THIS connection, asked now: no cache, so no window in which
        // an address this machine has lost still counts as this machine.
        if (IsLoopbackHost(peer.host))
            return std::nullopt;
        return Cc::Refuse(
            _metrics, MintNotLocal, "a machine ticket is minted for this machine's own processes only: ask over loopback");
    }
    return RefuseVerb(opRaw);
}

CredentialVerdict SessionResponder::CheckCredential(std::span<std::byte const> payload) const
{
    // The ticket is caught FIRST. The shared password rule answers every credential `NoPolicy` on a
    // surface that checks no password, a ticket included -- so asked first, a ticket would be
    // answered `Ok` and establish nothing, in silence.
    auto const fields = Wire::DecodeAuthPayload(payload);
    if (fields.has_value() && fields->kind == Wire::AuthKind::MachineTicket)
    {
        auto answer = Distributed::AnswerTicket(_metrics, _verifier.Verify(fields->secret, _wallClock.now()));
        if (answer.machine.has_value())
            return CredentialVerdict { .outcome = CredentialOutcome::Accepted, .machine = std::move(answer.machine) };
        // No machine, and -- for a revoked key -- the evidence that the presenter is the machine a
        // forget removed: a refused ticket grants nothing, and this one refuses what follows it.
        return CredentialVerdict { .outcome = CredentialOutcome::Rejected,
                                   .machine = std::nullopt,
                                   .revokedMachine = std::move(answer.revoked),
                                   .refusalReply = std::move(answer.refusalReply) };
    }

    // No policy: the node checks no password. The shared decision still tells a payload that will
    // not decode apart from one that did, so a client version skew is answered as one.
    return CredentialVerdict { .outcome = FastCache::CheckCredential(nullptr, payload) };
}

std::vector<std::byte> SessionResponder::Mint(std::span<std::byte const> payload) const
{
    auto audience = Wire::DecodeMintTicketPayload(payload);
    if (!audience.has_value())
        return Cc::Refuse(_metrics, MintMalformed, "the audience would not decode");

    auto claims = Distributed::MachineTicketClaims { .machineId = _machineId,
                                                     .audience = *std::move(audience),
                                                     .expiresAtUnixSeconds =
                                                         UnixSeconds(_wallClock.now() + Distributed::MachineTicketLifetime),
                                                     .nonce = {} };

    // The check the verifier refuses `not-utf8` by, asked before anything is signed -- and before
    // the audience is echoed into a message below, which must stay text.
    if (auto const field = Distributed::FirstClaimNotText(claims); field.has_value())
        return Cc::Refuse(
            _metrics, MintMalformed, std::format("the ticket's {} is not UTF-8 text, which every node refuses", *field));

    // The rule `AudienceMatches` refuses to spend by, asked of the same predicate: a ticket for a
    // loopback or wildcard host would be spendable at any node that heard it presented.
    if (!Distributed::AudienceNamesOneMachine(claims.audience))
        return Cc::Refuse(_metrics,
                          MintMalformed,
                          std::format("a ticket for {} would be spendable at any node: name the machine you are dialling",
                                      claims.audience));

    if (_identityKey == nullptr)
        return Cc::Refuse(_metrics, MintNoKey, "this node holds no identity key to sign a ticket with");

    // A failed draw is a REFUSAL (#1527), never a ticket with a weaker nonce.
    if (auto const drawn = _random.Fill(claims.nonce); !drawn.has_value())
        return Cc::Refuse(_metrics,
                          MintNoRandom,
                          std::format("this node's random source ({}) could not draw a nonce: {}",
                                      drawn.error().primitive,
                                      drawn.error().detail));

    SecureString const machineTicket = Distributed::MintMachineTicket(*_identityKey, claims);
    _metrics.Increment(IMetricsSink::Counter::NodeTicketsMinted);
    return Wire::EncodeReply(Wire::Status::Ok, Wire::AsBytes(machineTicket.View()));
}

std::vector<std::byte> SessionResponder::RefusalReply(Wire::PrePayloadDecision decision,
                                                      std::uint8_t /*opRaw*/,
                                                      std::string_view detail) const
{
    return Cc::RefuseWithoutCounter({ .code = Wire::ErrorCodeFor(decision),
                                      .rationale = decision == Wire::PrePayloadDecision::Unauthenticated
                                                       ? NodeChecksNoPasswordRationale
                                                       : SessionRefusalRationale },
                                    detail);
}

std::vector<std::byte> SessionResponder::EndpointRefusalReply(EndpointRefusal refusal,
                                                              std::uint8_t /*opRaw*/,
                                                              std::string_view detail) const
{
    auto const& row = SessionEndpointRefusals[static_cast<std::size_t>(refusal)];
    return AnswerEndpointRefusal(_metrics, ErrorCodeFor(refusal), std::nullopt, row.rationale, detail);
}

} // namespace FastCache::Node
