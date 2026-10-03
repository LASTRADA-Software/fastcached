// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/IdentityKeyLabel.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/TicketVerifier.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace FastCache::Distributed
{

namespace
{
    /// The claims another machine will read, and that therefore have to be text: a ticket's id is
    /// what an admission reports and the fleet renders, and its audience is an endpoint.
    constexpr std::array ClaimTextFields {
        TextField<MachineTicketClaims> {
            .name = "machine id", .project = [](MachineTicketClaims const& c) -> std::string_view { return c.machineId; } },
        TextField<MachineTicketClaims> {
            .name = "audience", .project = [](MachineTicketClaims const& c) -> std::string_view { return c.audience; } },
    };

    /// The one parse of an audience: its host and port when it names one machine.
    /// @param audience The endpoint a ticket names.
    /// @return Its host and port, or nullopt when it is no dial endpoint or names every node.
    [[nodiscard]] std::optional<std::pair<std::string, std::uint16_t>> OneMachineEndpoint(std::string_view audience)
    {
        auto parsed = ParseDialEndpoint(audience);
        if (!parsed.has_value() || NamesNoOneMachine(parsed->first))
            return std::nullopt;
        return parsed;
    }

    /// @param claims A decoded ticket's claims.
    /// @return Its expiry as an instant. `DecodeMachineTicket` refused anything past
    ///         `MaxMachineTicketExpirySeconds`, so the conversion cannot overflow.
    [[nodiscard]] std::chrono::system_clock::time_point ExpiryOf(MachineTicketClaims const& claims) noexcept
    {
        return std::chrono::system_clock::time_point { std::chrono::seconds {
            static_cast<std::int64_t>(claims.expiresAtUnixSeconds) } };
    }
} // namespace

bool AudienceNamesOneMachine(std::string_view audience)
{
    return OneMachineEndpoint(audience).has_value();
}

std::optional<std::string_view> FirstClaimNotText(MachineTicketClaims const& claims)
{
    return FirstFieldNotText(claims, ClaimTextFields);
}

bool AudienceMatches(std::string_view audience, OwnAudience const& own, ILocalityOracle const& locality)
{
    auto const endpoint = OneMachineEndpoint(audience);
    if (!endpoint.has_value())
        return false;
    auto const& [host, port] = *endpoint;
    if (!std::ranges::contains(own.ports, port))
        return false;
    return std::ranges::any_of(own.names, [&host](std::string const& name) { return EqualsIgnoringAsciiCase(name, host); })
           || locality.IsThisMachine(host);
}

SpentTickets::SpentTickets(std::size_t capacity) noexcept:
    _capacity { capacity }
{
}

SpendOutcome SpentTickets::Spend(std::span<std::byte const> claims,
                                 std::chrono::system_clock::time_point expiresAt,
                                 std::chrono::system_clock::time_point now)
{
    auto const entry = Entry { expiresAt, Sha256::Hash(claims) };
    std::scoped_lock const guard { _mutex };

    // Only what the window has PASSED leaves, and only from the front, where the earliest expiries
    // are. An entry too far ahead of this clock stays: the clock stepped back, and dropping it
    // would make its ticket replayable once the clock stepped forward again.
    while (!_spent.empty() && _spent.begin()->first < now && !TicketAcceptable(_spent.begin()->first, now))
    {
        _forgottenThrough = std::max(_forgottenThrough, _spent.begin()->first);
        _spent.erase(_spent.begin());
    }

    if (_spent.contains(entry))
        return SpendOutcome::AlreadySpent;
    // An expiry this set has already seen leave the window: it may be a spend it forgot, which
    // cannot be told apart from a fresh ticket, so it is refused rather than trusted.
    if (expiresAt <= _forgottenThrough)
        return SpendOutcome::WindowPassed;
    if (_spent.size() >= _capacity)
        return SpendOutcome::Full;
    _spent.insert(entry);
    return SpendOutcome::Spent;
}

std::size_t SpentTickets::Size() const
{
    std::scoped_lock const guard { _mutex };
    return _spent.size();
}

TicketVerifier::TicketVerifier(ILeaseRoster const* roster, IAudience const& audience, SpentTickets& spent) noexcept:
    _roster { roster },
    _audience { audience },
    _spent { spent }
{
}

std::expected<ProvenIdentity, TicketRejection> TicketVerifier::Verify(std::span<std::byte const> ticket,
                                                                      std::chrono::system_clock::time_point now) const
{
    auto const decoded = DecodeMachineTicket(ticket);
    if (!decoded.has_value())
        return std::unexpected { TicketRejection { TicketRefusal::Malformed } };

    // The signature is checked over the message REBUILT from the claims, through the labelled seam
    // every identity-key construction verifies through. The decoder admits exactly one encoding of
    // those claims, so the rebuilt bytes are the received `signedBytes` -- which stay what a spend
    // is keyed on. That equality is ASSERTED rather than relied on: were a later decoder to accept
    // a second encoding of the same claims, the signature would be verified over one byte string
    // and the spend keyed on another, and a ticket could be replayed in its other spelling. Refused
    // as malformed, beside the decode, since it is a fact about the bytes and nothing a signer made.
    auto const message = MachineTicketMessage(decoded->claims);
    if (!std::ranges::equal(message.Bytes(), decoded->signedBytes))
        return std::unexpected { TicketRejection { TicketRefusal::Malformed } };

    if (_roster == nullptr || _roster->Read(now).standing != RosterStanding::Current)
        return std::unexpected { TicketRejection { TicketRefusal::NoRoster } };

    // The signature is the first claim read; the id only SELECTS the key. And the revoked list is
    // asked outright rather than trusting `live` to have been filtered against it: a roster that
    // answers a key both live and revoked has been told the machine is gone.
    auto const keys = _roster->MachineKeysOf(decoded->claims.machineId);
    auto const signedBy = [&decoded, &message](Ed25519PublicKey const& key) {
        return VerifyLabelled(key, message, decoded->signature);
    };
    auto const live = keys.live.has_value() && !std::ranges::contains(keys.revoked, *keys.live) ? keys.live : std::nullopt;
    if (!live.has_value() || !signedBy(*live))
    {
        // The revoked key is KEPT: the presenter is the forgotten machine, and saying so is what
        // refuses the commands it pipelined behind this AUTH wherever it dials from.
        if (auto const revoked = std::ranges::find_if(keys.revoked, signedBy); revoked != keys.revoked.end())
            return std::unexpected { TicketRejection::RevokedKey(
                RevokedKeyEvidence { ProvenIdentity { .id = decoded->claims.machineId, .key = *revoked } }) };
        if (live.has_value())
            return std::unexpected { TicketRejection { TicketRefusal::Forged } };
        return std::unexpected { TicketRejection { TicketRefusal::UnknownMachine } };
    }

    // Refused before either claim is compared, logged or rendered: one byte that is not UTF-8
    // makes a fleet document unparseable for every reader. AFTER the signature, because the claims
    // of a ticket nobody signed are an attacker's to choose -- asked first, they would steer the
    // answer away from `forged`, and move this counter for anyone who can reach the port.
    if (FirstClaimNotText(decoded->claims).has_value())
        return std::unexpected { TicketRejection { TicketRefusal::NotUtf8 } };

    if (!_audience.Matches(decoded->claims.audience))
        return std::unexpected { TicketRejection { TicketRefusal::WrongAudience } };

    auto const expiresAt = ExpiryOf(decoded->claims);
    if (!TicketAcceptable(expiresAt, now))
        return std::unexpected { TicketRejection { TicketRefusal::Expired } };

    // LAST, so a ticket refused for anything above is never spent, and occupies no capacity.
    // Keyed by the signed CLAIMS, so one set of claims is one spend however the signature is encoded.
    switch (_spent.Spend(decoded->signedBytes, expiresAt, now))
    {
        case SpendOutcome::Spent:
            break;
        case SpendOutcome::AlreadySpent:
            return std::unexpected { TicketRejection { TicketRefusal::Replayed } };
        case SpendOutcome::Full:
            return std::unexpected { TicketRejection { TicketRefusal::SpentSetFull } };
        case SpendOutcome::WindowPassed:
            return std::unexpected { TicketRejection { TicketRefusal::Expired } };
    }

    return ProvenIdentity { .id = decoded->claims.machineId, .key = *live };
}

std::vector<std::byte> AnswerTicketRefusal(IMetricsSink& metrics, TicketRefusal refusal)
{
    auto const& row = DescribeTicketRefusal(refusal);
    return Cc::Refuse(
        metrics, { .code = CompileCacheWire::ErrorCode::TicketRefused, .counter = row.counter }, TicketRefusalMessage(row));
}

TicketAnswer AnswerTicket(IMetricsSink& metrics, std::expected<ProvenIdentity, TicketRejection> verified)
{
    if (!verified.has_value())
        return TicketAnswer { .machine = std::nullopt,
                              .revoked = verified.error().Revoked(),
                              .refusalReply = AnswerTicketRefusal(metrics, verified.error().Reason()) };
    metrics.Increment(IMetricsSink::Counter::NodeTicketsAccepted);
    return TicketAnswer { .machine = *std::move(verified), .revoked = std::nullopt, .refusalReply = {} };
}

} // namespace FastCache::Distributed
