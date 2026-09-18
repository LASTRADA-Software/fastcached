// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cluster
{

namespace
{
    /// Fields in an encoded endorsement: cluster, version, digest, not-after, endorser, signature.
    constexpr std::size_t EndorsementFields = 6;

    /// Fields in an encoded certified roster: the layout version, cluster, version, the roster
    /// and the endorsements.
    constexpr std::size_t CertifiedRosterFields = 5;

    /// Fields in a kept roster: the layout version, the certified roster, and its lapse.
    constexpr std::size_t PersistedRosterFields = 3;

    /// Milliseconds since the epoch, as the wire carries an instant.
    /// @param instant The instant.
    /// @return Its big-endian bytes.
    [[nodiscard]] std::array<std::byte, sizeof(std::uint64_t)> InstantBytes(std::chrono::system_clock::time_point instant)
    {
        return WireFields::ToBigEndian<std::uint64_t>(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(instant.time_since_epoch()).count()));
    }

    /// The largest instant this host's clock can represent, in milliseconds -- the lease token's
    /// bound, for its reason: a count near `uint64`'s ceiling overflows the conversion.
    constexpr std::int64_t MaxInstantMillis =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::duration::max()).count();

    /// Read an instant written by `InstantBytes`.
    /// @param field The field.
    /// @return The instant, or nothing when it is not one.
    [[nodiscard]] std::optional<std::chrono::system_clock::time_point> ReadInstant(std::span<std::byte const> field)
    {
        auto const millis = WireFields::FromBigEndian<std::uint64_t>(field);
        if (!millis.has_value() || *millis > static_cast<std::uint64_t>(MaxInstantMillis))
            return std::nullopt;
        return std::chrono::system_clock::time_point { std::chrono::milliseconds { static_cast<std::int64_t>(*millis) } };
    }

    /// Whether an endorsement claims a `notAfter` further ahead than one lifetime and the slack,
    /// which no honest voter writes -- and which would otherwise let one endorsement outlive every
    /// refresh. Such an endorsement counts for NOTHING, and is not a lapsed one either.
    /// @param notAfter The endorsement's.
    /// @param now This clock.
    /// @param slack How far this clock may trail the endorser's.
    /// @return True when it claims too much.
    [[nodiscard]] bool ClaimsTooLong(std::chrono::system_clock::time_point notAfter,
                                     std::chrono::system_clock::time_point now,
                                     std::chrono::seconds slack) noexcept
    {
        // Comparisons before subtractions, for the lease token's overflow reasoning: `notAfter`
        // is a peer's claim and `now` may be anything this host's clock says.
        return notAfter > now && notAfter - now > RosterEndorsementLifetime + slack;
    }

    /// Whether an endorsement has lapsed at @p now: past its `notAfter` by more than the slack.
    /// @param notAfter The endorsement's.
    /// @param now This clock.
    /// @param slack How far this clock may trail the endorser's.
    /// @return True once it no longer counts.
    [[nodiscard]] bool Lapsed(std::chrono::system_clock::time_point notAfter,
                              std::chrono::system_clock::time_point now,
                              std::chrono::seconds slack) noexcept
    {
        return now > notAfter && now - notAfter > slack;
    }
} // namespace

std::vector<std::byte> EndorsementMessage(RosterEndorsement const& endorsement)
{
    auto const version = WireFields::ToBigEndian<std::uint64_t>(endorsement.version);
    auto const notAfter = InstantBytes(endorsement.notAfter);
    return WireFields::Encode({ WireFields::AsBytes(RosterEndorsementLabel),
                                WireFields::AsBytes(endorsement.clusterId),
                                std::span<std::byte const> { version },
                                std::span<std::byte const> { endorsement.rosterDigest },
                                std::span<std::byte const> { notAfter },
                                WireFields::AsBytes(endorsement.endorser) });
}

RosterEndorsement SignEndorsement(RosterEndorsement claims,
                                  std::function<Ed25519Signature(std::span<std::byte const>)> const& sign)
{
    claims.signature = sign(EndorsementMessage(claims));
    return claims;
}

bool VerifyEndorsement(RosterEndorsement const& endorsement, Ed25519PublicKey const& key)
{
    return Ed25519Verify(key, EndorsementMessage(endorsement), endorsement.signature);
}

std::vector<std::byte> EncodeEndorsement(RosterEndorsement const& endorsement)
{
    auto const version = WireFields::ToBigEndian<std::uint64_t>(endorsement.version);
    auto const notAfter = InstantBytes(endorsement.notAfter);
    return WireFields::Encode({ WireFields::AsBytes(endorsement.clusterId),
                                std::span<std::byte const> { version },
                                std::span<std::byte const> { endorsement.rosterDigest },
                                std::span<std::byte const> { notAfter },
                                WireFields::AsBytes(endorsement.endorser),
                                std::span<std::byte const> { endorsement.signature } });
}

std::expected<RosterEndorsement, ConsensusError> DecodeEndorsement(std::span<std::byte const> bytes)
{
    auto const fields = WireFields::SplitExactly(bytes, EndorsementFields);
    if (!fields.has_value())
        return std::unexpected(MalformedWireFrame("the bytes are not a roster endorsement"));
    auto const version = WireFields::FromBigEndian<std::uint64_t>((*fields)[1]);
    auto const notAfter = ReadInstant((*fields)[3]);
    if (!version.has_value() || !notAfter.has_value()
        || (*fields)[2].size() != std::tuple_size_v<RosterDigest> || (*fields)[5].size() != Ed25519SignatureBytes)
        return std::unexpected(MalformedWireFrame("a roster endorsement's fields are the wrong width"));

    auto endorsement = RosterEndorsement { .clusterId = std::string { WireFields::AsStringView((*fields)[0]) },
                                           .version = *version,
                                           .rosterDigest = {},
                                           .notAfter = *notAfter,
                                           .endorser = std::string { WireFields::AsStringView((*fields)[4]) },
                                           .signature = {} };
    std::ranges::copy((*fields)[2], endorsement.rosterDigest.begin());
    std::ranges::copy((*fields)[5], endorsement.signature.begin());
    return endorsement;
}

std::vector<std::byte> EncodeCertifiedRoster(CertifiedRoster const& certified)
{
    std::vector<std::vector<std::byte>> encoded;
    encoded.reserve(certified.endorsements.size());
    for (auto const& endorsement: certified.endorsements)
        encoded.push_back(EncodeEndorsement(endorsement));
    std::vector<std::span<std::byte const>> spans;
    spans.reserve(encoded.size());
    for (auto const& bytes: encoded)
        spans.emplace_back(bytes);
    auto const endorsements = WireFields::Encode(WireFields::FieldList { spans });

    auto const format = std::array { static_cast<std::byte>(CertifiedRosterFormatVersion) };
    auto const version = WireFields::ToBigEndian<std::uint64_t>(certified.version);
    return WireFields::Encode({ std::span<std::byte const> { format },
                                WireFields::AsBytes(certified.clusterId),
                                std::span<std::byte const> { version },
                                std::span<std::byte const> { certified.roster },
                                std::span<std::byte const> { endorsements } });
}

std::expected<CertifiedRoster, ConsensusError> DecodeCertifiedRoster(std::span<std::byte const> bytes)
{
    auto const fields = WireFields::SplitAll(bytes);
    if (!fields.has_value() || fields->empty() || (*fields)[0].size() != 1)
        return std::unexpected(MalformedWireFrame("the bytes are not a certified roster"));
    if (auto const format = static_cast<std::uint8_t>((*fields)[0][0]); format != CertifiedRosterFormatVersion)
        return std::unexpected(UnsupportedWireVersion(std::format(
            "certified roster encoding version {} (this build reads {})", format, CertifiedRosterFormatVersion)));
    if (fields->size() != CertifiedRosterFields)
        return std::unexpected(MalformedWireFrame("a certified roster is not its five fields"));

    auto const version = WireFields::FromBigEndian<std::uint64_t>((*fields)[2]);
    auto const records = WireFields::SplitAll((*fields)[4]);
    if (!version.has_value() || !records.has_value())
        return std::unexpected(MalformedWireFrame("a certified roster's version or endorsements are malformed"));

    CertifiedRoster certified { .clusterId = std::string { WireFields::AsStringView((*fields)[1]) },
                                .version = *version,
                                .roster = { (*fields)[3].begin(), (*fields)[3].end() },
                                .endorsements = {} };
    certified.endorsements.reserve(records->size());
    for (auto const record: *records)
    {
        auto endorsement = DecodeEndorsement(record);
        if (!endorsement.has_value())
            return std::unexpected(std::move(endorsement).error());
        certified.endorsements.push_back(*std::move(endorsement));
    }
    return certified;
}

std::vector<std::byte> EncodePersistedRoster(PersistedRoster const& persisted)
{
    auto const format = std::array { static_cast<std::byte>(PersistedRosterFormatVersion) };
    auto const certificate = EncodeCertifiedRoster(persisted.certificate);
    auto const until = InstantBytes(persisted.certifiedUntil);
    return WireFields::Encode({ std::span<std::byte const> { format },
                                std::span<std::byte const> { certificate },
                                std::span<std::byte const> { until } });
}

std::expected<PersistedRoster, ConsensusError> DecodePersistedRoster(std::span<std::byte const> bytes)
{
    // The version first, and before the arity: a later layout may have more fields, and it is
    // the version that says so.
    auto const fields = WireFields::SplitAll(bytes);
    if (!fields.has_value() || fields->empty() || (*fields)[0].size() != 1)
        return std::unexpected(MalformedWireFrame("the bytes are not a kept roster"));
    if (auto const format = static_cast<std::uint8_t>((*fields)[0][0]); format != PersistedRosterFormatVersion)
        return std::unexpected(UnsupportedWireVersion(
            std::format("kept roster layout version {} (this build reads {})", format, PersistedRosterFormatVersion)));
    if (fields->size() != PersistedRosterFields)
        return std::unexpected(MalformedWireFrame("a kept roster is not its three fields"));

    auto certificate = DecodeCertifiedRoster((*fields)[1]);
    if (!certificate.has_value())
        return std::unexpected(std::move(certificate).error());
    auto const until = ReadInstant((*fields)[2]);
    if (!until.has_value())
        return std::unexpected(MalformedWireFrame("a kept roster's lapse is not an instant"));
    return PersistedRoster { .certificate = *std::move(certificate), .certifiedUntil = *until };
}

CertifyingVoters VotersOf(Roster const& roster)
{
    auto const revoked = [&roster](Ed25519PublicKey const& key) {
        return std::ranges::contains(roster.revoked, key, &RevokedKey::publicKey);
    };
    CertifyingVoters voters;
    for (auto const& member: roster.members)
    {
        if (member.seat != MemberSeat::Voter)
            continue;
        ++voters.voters;
        if (member.publicKey.has_value() && !revoked(*member.publicKey))
            voters.endorsers.push_back(TrustedEndorser { .id = member.id, .key = *member.publicKey });
    }
    return voters;
}

CertifyingVoters AnchorVoters(std::span<Ed25519PublicKey const> anchors)
{
    CertifyingVoters voters;
    for (auto const& key: anchors)
        if (!std::ranges::contains(voters.endorsers, key, &TrustedEndorser::key))
            voters.endorsers.push_back(TrustedEndorser { .id = std::nullopt, .key = key });
    voters.voters = voters.endorsers.size();
    return voters;
}

std::expected<AdoptedRoster, RosterRefusal> CertifyRoster(CertifiedRoster const& offered, CertificationInput const& input)
{
    if (offered.clusterId != input.clusterId)
        return std::unexpected(RosterRefusal::WrongCluster);
    if (offered.version < input.minimumVersion)
        return std::unexpected(RosterRefusal::Stale);

    // Over the bytes AS RECEIVED, before they are decoded: an endorsement vouches for the
    // digest, so only a digest of what was sent can make it vouch for what was sent.
    auto const digest = DigestOfRoster(offered.roster);
    if (offered.version == input.minimumVersion && input.held.has_value() && *input.held != digest)
        return std::unexpected(RosterRefusal::Conflicting);
    auto roster = DecodeRoster(offered.roster);
    if (!roster.has_value())
        return std::unexpected(RosterRefusal::Malformed);

    // Each certifying voter counts ONCE, with the latest `notAfter` it signed that still holds;
    // an endorsement that verifies but has lapsed is remembered only to tell `Expired` from
    // `Uncertified`.
    std::vector<std::chrono::system_clock::time_point> live;
    std::size_t lapsed = 0;
    for (auto const& endorser: input.voters.endorsers)
    {
        auto best = std::optional<std::chrono::system_clock::time_point> {};
        auto sawLapsed = false;
        for (auto const& endorsement: offered.endorsements)
        {
            if (endorsement.clusterId != offered.clusterId || endorsement.version != offered.version
                || endorsement.rosterDigest != digest)
                continue;
            // Compared as the optional, so a disengaged id -- an anchor, which names nobody --
            // is never dereferenced: it is skipped by the `has_value` and matches any endorser.
            if (endorser.id.has_value() && endorser.id != endorsement.endorser)
                continue;
            if (!VerifyEndorsement(endorsement, endorser.key) || ClaimsTooLong(endorsement.notAfter, input.now, input.slack))
                continue;
            if (Lapsed(endorsement.notAfter, input.now, input.slack))
            {
                sawLapsed = true;
                continue;
            }
            if (!best.has_value() || endorsement.notAfter > *best)
                best = endorsement.notAfter;
        }
        if (best.has_value())
            live.push_back(*best);
        else if (sawLapsed)
            ++lapsed;
    }

    auto const needed = (input.voters.voters / 2) + 1;
    if (input.voters.voters == 0 || live.size() < needed)
        return std::unexpected(live.size() + lapsed >= needed && input.voters.voters != 0 ? RosterRefusal::Expired
                                                                                          : RosterRefusal::Uncertified);

    // Certified until the majority lapses: the `needed`-th latest `notAfter`.
    std::ranges::sort(live, std::ranges::greater {});
    return AdoptedRoster {
        .certificate = offered, .roster = *std::move(roster), .digest = digest, .certifiedUntil = live[needed - 1]
    };
}

} // namespace FastCache::Cluster
