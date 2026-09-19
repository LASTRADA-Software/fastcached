// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/RosterTrust.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

namespace
{
    /// Summarise a roster, with or without a certificate.
    /// @param roster The roster.
    /// @param version Its version.
    /// @param certifiedUntil When its certificate lapses, if it has one.
    /// @return The summary.
    [[nodiscard]] RosterSummary Summarise(Cluster::Roster const& roster,
                                          std::uint64_t version,
                                          std::optional<std::chrono::system_clock::time_point> certifiedUntil)
    {
        auto const voters = std::ranges::count(roster.members, Cluster::MemberSeat::Voter, &Cluster::RosterMember::seat);
        return RosterSummary { .version = version,
                               .voters = static_cast<std::uint32_t>(voters),
                               .principals = static_cast<std::uint32_t>(roster.principals.size()),
                               .revoked = static_cast<std::uint32_t>(roster.revoked.size()),
                               .certifiedUntil = certifiedUntil };
    }
} // namespace

RosterTrust::RosterTrust(std::optional<std::string> assertedClusterId,
                         std::vector<Ed25519PublicKey> anchors,
                         std::optional<Cluster::PersistedRoster> persisted,
                         IRosterStore* store,
                         IMetricsSink& metrics,
                         ILogger& logger,
                         std::chrono::seconds slack):
    _assertedClusterId { std::move(assertedClusterId) },
    _anchors { std::move(anchors) },
    _store { store },
    _metrics { metrics },
    _logger { logger },
    _slack { slack }
{
    // A roster this worker kept is ITS record, adopted under the rule on an earlier run, so it is
    // taken as it was kept -- certification lapse included, which a restart must not reset.
    if (persisted.has_value())
    {
        auto roster = Cluster::DecodeRoster(persisted->certificate.roster);
        if (roster.has_value())
            _held = Cluster::AdoptedRoster { .certificate = std::move(persisted->certificate),
                                             .roster = *std::move(roster),
                                             .digest = {},
                                             .certifiedUntil = persisted->certifiedUntil };
        if (_held.has_value())
            _held->digest = Cluster::DigestOfRoster(_held->certificate.roster);
    }
}

std::optional<std::string_view> RosterTrust::ExpectedClusterLocked() const
{
    if (_held.has_value())
        return std::string_view { _held->certificate.clusterId };
    if (_assertedClusterId.has_value())
        return std::string_view { *_assertedClusterId };
    return std::nullopt;
}

Cluster::CertifyingVoters RosterTrust::CertifyingVotersLocked() const
{
    return _held.has_value() ? Cluster::VotersOf(_held->roster) : Cluster::AnchorVoters(_anchors);
}

RosterOfferOutcome RosterTrust::Offer(Cluster::CertifiedRoster const& offered, std::chrono::system_clock::time_point now)
{
    std::unique_lock const lock { _lock };

    auto const voters = CertifyingVotersLocked();
    auto const minimum = _held.has_value() ? _held->certificate.version : std::uint64_t { 0 };
    auto const held = _held.has_value() ? std::optional { _held->digest } : std::nullopt;
    auto adopted = Cluster::CertifyRoster(
        offered,
        Cluster::CertificationInput { .clusterId = ExpectedClusterLocked().value_or(offered.clusterId),
                                      .voters = voters,
                                      .minimumVersion = minimum,
                                      .held = held,
                                      .now = now,
                                      .slack = _slack });
    if (!adopted.has_value())
    {
        auto const refusal = adopted.error();
        // A stale roster is what an ex-leader or a slow one serves for a round or two: ordinary,
        // never counted and never said.
        if (refusal == Cluster::RosterRefusal::Stale)
            return RosterOfferOutcome::Kept;

        // Two statements rather than one choosing its argument, so each row is written where
        // `ctest -R counter-attribution` can see it written.
        if (refusal == Cluster::RosterRefusal::Expired)
            _metrics.Increment(IMetricsSink::Counter::WorkerRostersRefusedExpired);
        else
            _metrics.Increment(IMetricsSink::Counter::WorkerRostersRefusedUncertified);
        if (_lastSaid != refusal)
        {
            _lastSaid = refusal;
            _logger.Logf(LogLevel::Warn,
                         "roster: refused roster version {} for cluster '{}': {}; keeping the one this worker holds "
                         "(every refusal is counted; this line repeats only when the reason changes)",
                         offered.version,
                         offered.clusterId,
                         Cluster::DescribeRosterRefusal(refusal));
        }
        return RosterOfferOutcome::Refused;
    }
    _lastSaid.reset();

    // The same roster, re-endorsed, lasts longer; one that is not newer and lasts no longer is
    // not worth writing down again.
    auto const newer = !_held.has_value() || adopted->certificate.version > _held->certificate.version;
    if (!newer && adopted->certifiedUntil <= _held->certifiedUntil)
        return RosterOfferOutcome::Kept;

    if (newer)
        _logger.Logf(LogLevel::Info,
                     "roster: adopted roster version {} ({}), certified until {:%FT%TZ}",
                     adopted->certificate.version,
                     Cluster::RenderRosterFingerprint(adopted->digest),
                     std::chrono::floor<std::chrono::seconds>(adopted->certifiedUntil));

    if (_store != nullptr)
        if (auto const saved = _store->Save(
                Cluster::PersistedRoster { .certificate = adopted->certificate, .certifiedUntil = adopted->certifiedUntil });
            !saved.has_value())
            _logger.Logf(LogLevel::Warn,
                         "roster: could not keep roster version {}: {}; a restart falls back to the one kept before",
                         adopted->certificate.version,
                         saved.error());

    _held = *std::move(adopted);
    return newer ? RosterOfferOutcome::Adopted : RosterOfferOutcome::Refreshed;
}

LeaseSignerKeys RosterTrust::KeysOf(std::string_view signer) const
{
    std::shared_lock const lock { _lock };
    if (!_held.has_value())
        return {};

    auto keys = LeaseSignerKeys {};
    for (auto const& revoked: _held->roster.revoked)
        keys.revoked.push_back(revoked.publicKey);
    for (auto const& member: _held->roster.members)
        if (member.id == signer && member.seat == Cluster::MemberSeat::Voter && member.publicKey.has_value()
            && !std::ranges::contains(keys.revoked, *member.publicKey))
            keys.live = member.publicKey;
    return keys;
}

RosterReading RosterTrust::Read(std::chrono::system_clock::time_point now) const
{
    std::shared_lock const lock { _lock };
    if (!_held.has_value())
        return RosterReading { .standing = RosterStanding::Absent, .certifiedUntil = std::nullopt };

    // The lease's acceptance predicate, for its overflow reasoning and so a roster and a grant
    // cannot disagree about what "past its time and the slack" means.
    auto const current = Detail::WithinAcceptanceWindow(_held->certifiedUntil, now, _slack);
    return RosterReading { .standing = current ? RosterStanding::Current : RosterStanding::Expired,
                           .certifiedUntil = _held->certifiedUntil };
}

std::optional<RosterSummary> RosterTrust::Summary() const
{
    std::shared_lock const lock { _lock };
    if (!_held.has_value())
        return std::nullopt;
    return Summarise(_held->roster, _held->certificate.version, _held->certifiedUntil);
}

void StateLeaseRoster::Adopt(Cluster::ClusterState const& state)
{
    std::map<std::string, Ed25519PublicKey, std::less<>> voters;
    for (auto const& member: state.members)
        if (member.seat == Cluster::MemberSeat::Voter && member.publicKey.has_value())
            voters.emplace(member.id, *member.publicKey);
    std::vector<Ed25519PublicKey> revoked;
    revoked.reserve(state.revokedKeys.size());
    for (auto const& entry: state.revokedKeys)
        revoked.push_back(entry.publicKey);
    auto summary = Summarise(Cluster::ProjectRoster(state), state.rosterVersion, std::nullopt);

    std::unique_lock const lock { _lock };
    _voters = std::move(voters);
    _revoked = std::move(revoked);
    _summary = summary;
}

LeaseSignerKeys StateLeaseRoster::KeysOf(std::string_view signer) const
{
    std::shared_lock const lock { _lock };
    auto keys = LeaseSignerKeys { .live = std::nullopt, .revoked = _revoked };
    if (auto const voter = _voters.find(signer); voter != _voters.end())
        keys.live = voter->second;
    return keys;
}

RosterReading StateLeaseRoster::Read(std::chrono::system_clock::time_point now) const
{
    (void) now;
    return RosterReading { .standing = RosterStanding::Current, .certifiedUntil = std::nullopt };
}

RosterSummary StateLeaseRoster::Summary() const
{
    std::shared_lock const lock { _lock };
    return _summary;
}

bool StateLeaseRoster::HoldsVoterKeys() const
{
    std::shared_lock const lock { _lock };
    return !_voters.empty();
}

} // namespace FastCache::Distributed
