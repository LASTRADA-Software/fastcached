// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Distributed/StateLeaseRoster.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Distributed
{

StateLeaseRoster::StateLeaseRoster(core::platform::IClock const& clock):
    _clock { clock },
    _lastLeaderContact { clock.now() }
{
}

void StateLeaseRoster::NoteLeaderReading(LeaderReading const& reading)
{
    if (!reading.silentFor.has_value())
        return;
    // A negative age is a reporter whose clock disagrees with ours; it is read as "just now",
    // never as a contact in the future that would hold the bound off past its time.
    auto const age = std::max(*reading.silentFor, core::platform::SteadyTimePoint::duration::zero());
    std::unique_lock const lock { _lock };
    auto const counted = reading.leads || (reading.leader.has_value() && std::ranges::contains(_voterIds, *reading.leader));
    if (counted)
        _lastLeaderContact = std::max(_lastLeaderContact, _clock.now() - age);
}

bool StateLeaseRoster::Isolated() const
{
    std::shared_lock const lock { _lock };
    return _clock.now() - _lastLeaderContact > LeaderSilenceBound;
}

void StateLeaseRoster::Adopt(Cluster::ClusterState const& state)
{
    std::map<std::string, Ed25519PublicKey, std::less<>> voters;
    std::map<std::string, Ed25519PublicKey, std::less<>> machines;
    std::vector<std::string> voterIds;
    for (auto const& member: state.members)
    {
        machines.emplace(member.id, member.publicKey);
        if (member.seat != Cluster::MemberSeat::Voter)
            continue;
        voterIds.push_back(member.id);
        voters.emplace(member.id, member.publicKey);
    }
    std::vector<Ed25519PublicKey> revoked;
    revoked.reserve(state.revokedKeys.size());
    for (auto const& entry: state.revokedKeys)
        revoked.push_back(entry.publicKey);
    auto roster = Cluster::ProjectRoster(state);
    auto voterKeys = std::vector<Ed25519PublicKey> {};
    voterKeys.reserve(voters.size());
    for (auto const& [id, key]: voters)
        voterKeys.push_back(key);
    auto summary = RosterSummary {
        .version = state.rosterVersion,
        .voters = static_cast<std::uint32_t>(
            std::ranges::count(roster.members, Cluster::MemberSeat::Voter, &Cluster::RosterMember::seat)),
        .principals = static_cast<std::uint32_t>(roster.principals.size()),
        .revoked = static_cast<std::uint32_t>(roster.revoked.size()),
        .voterKeys = std::move(voterKeys),
    };

    std::unique_lock const lock { _lock };
    _voters = std::move(voters);
    _machines = std::move(machines);
    _voterIds = std::move(voterIds);
    _revoked = std::move(revoked);
    _summary = std::move(summary);
    _roster = std::move(roster);
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
    // The WALL clock is not what bounds this: contact is stamped on the steady clock consensus
    // runs on, so an NTP step can neither lapse the state nor revive it.
    (void) now;
    std::shared_lock const lock { _lock };
    if (_voters.empty())
        return RosterReading { .standing = RosterStanding::Absent };
    if (_clock.now() - _lastLeaderContact > LeaderSilenceBound)
        return RosterReading { .standing = RosterStanding::Isolated };
    return RosterReading { .standing = RosterStanding::Current };
}

LeaseSignerKeys StateLeaseRoster::MachineKeysOf(std::string_view machine) const
{
    std::shared_lock const lock { _lock };
    auto keys = LeaseSignerKeys { .live = std::nullopt, .revoked = _revoked };
    if (auto const found = _machines.find(machine); found != _machines.end())
        keys.live = found->second;
    return keys;
}

RosterSummary StateLeaseRoster::Summary() const
{
    std::shared_lock const lock { _lock };
    return _summary;
}

Cluster::Roster StateLeaseRoster::Held() const
{
    std::shared_lock const lock { _lock };
    return _roster;
}

bool StateLeaseRoster::HoldsVoterKeys() const
{
    std::shared_lock const lock { _lock };
    return !_voters.empty();
}

} // namespace FastCache::Distributed
