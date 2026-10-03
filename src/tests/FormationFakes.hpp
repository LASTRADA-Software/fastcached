// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cluster/ChallengeCookies.hpp>
#include <FastCache/Cluster/ChallengeIssuer.hpp>
#include <FastCache/Cluster/DiscoveryWire.hpp>
#include <FastCache/Cluster/FleetSummarySignature.hpp>
#include <FastCache/Cluster/FormationRecord.hpp>
#include <FastCache/Cluster/ProvenFleet.hpp>
#include <FastCache/Cluster/ProvenFleetSummary.hpp>
#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Core/Nonce.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>
#include <tests/RaftPeerKeyFakes.hpp>
#include <tests/SecureRandomFakes.hpp>
#include <tests/Unwrap.hpp>

/// @file FormationFakes.hpp
/// The shared fakes zero-config formation is tested through: what a node says about itself, where
/// a proven fleet goes, and a proven summary built the way discovery builds one.
///
/// Shared rather than written per file, for `MembershipFakes.hpp`'s reason: a WRONG fake makes
/// its cases pass, and the facts these carry -- that a proven summary is one a SIGNATURE verified,
/// that an observer is handed every fleet -- are exactly what a copy would get wrong.
///
/// Library-level only: `src/FastCache` tests include this, and must not reach an app header.
namespace FastCache::Testing
{

/// This node's summary, as a case scripts it: what `IFleetSummarySource::Current` answers until
/// the case changes it.
class ScriptedSummarySource final: public Cluster::IFleetSummarySource
{
  public:
    /// @param summary What this node says about itself.
    explicit ScriptedSummarySource(CompileCacheWire::FleetSummary summary):
        _summary { std::move(summary) }
    {
    }

    [[nodiscard]] CompileCacheWire::FleetSummary Current() const override
    {
        std::scoped_lock const lock { _lock };
        return _summary;
    }

    /// Say something else from now on: a node whose fleet changed.
    /// @param summary The new summary.
    void Set(CompileCacheWire::FleetSummary summary)
    {
        std::scoped_lock const lock { _lock };
        _summary = std::move(summary);
    }

  private:
    mutable std::mutex _lock;
    CompileCacheWire::FleetSummary _summary;
};

/// Every proven fleet it was handed, in order.
class RecordingFleets final: public Cluster::IFleetObserver
{
  public:
    void OnFleetProven(Cluster::ProvenFleet const& fleet) override
    {
        proven.push_back(fleet);
    }

    std::vector<Cluster::ProvenFleet> proven; ///< What arrived.
};

/// A one-use nonce every byte of which is @p fill, as `ChallengeIssuer::IssueNonce` draws it from a
/// scripted generator -- the only way a case, like production, comes to hold an `IssuedNonce`.
/// @param fill Every byte of the nonce.
/// @return The nonce to send and the one to verify the answer against.
[[nodiscard]] inline Cluster::FreshNonce IssuedNonceOf(std::byte fill)
{
    auto random = ScriptedSecureRandom { std::vector<std::byte>(NonceBytes, fill) };
    auto issuer = Cluster::ChallengeIssuer { random };
    auto fresh = issuer.IssueNonce();
    REQUIRE(fresh.has_value());
    return *std::move(fresh);
}

/// @p summary as the machine @p machine proves it: signed by `TestKeyPair(machine)` over a
/// challenge this side issued, and verified by the cookie jar that issued it -- the only way a
/// discovery `ProvenFleetSummary` exists.
/// @param summary What the machine says.
/// @param machine Whose key signs it.
/// @return The summary, verified.
[[nodiscard]] inline Cluster::ProvenFleetSummary ProvenBy(CompileCacheWire::FleetSummary const& summary,
                                                          std::string const& machine)
{
    auto random = ScriptedSecureRandom { ScriptedSecureRandom::Ascending(Cluster::ChallengeCookies::CookieKeyBytes) };
    auto cookies = Cluster::ChallengeCookies { random, std::chrono::seconds { 30 } };
    auto const now = core::platform::SteadyTimePoint {};
    auto const challenge = cookies.Issue(now, "asker", summary, "10.0.0.2");
    REQUIRE(challenge.has_value());
    auto const pair = TestKeyPair(machine);
    auto const proof = Cluster::DiscoveryWire::Proof {
        .summary = summary,
        .answers = challenge->nonce,
        .publicKey = pair.PublicKey(),
        .signature = SignLabelled(pair, Cluster::DiscoveryWire::ProofMessage(*challenge, summary, pair.PublicKey())),
    };
    auto const proven = cookies.Verify(now, "asker", proof, "10.0.0.2", [] { return true; });
    REQUIRE(proven.has_value());
    return Unwrap(proven);
}

/// @p summary as discovery hands it on: proven by the key of its node, arrived by a beacon.
/// @param summary What the other fleet's node says; its `nodeId` names the key, or its cluster id
///        when it names no node.
/// @return The proven fleet.
[[nodiscard]] inline Cluster::ProvenFleet ProvenBeacon(CompileCacheWire::FleetSummary const& summary)
{
    auto const machine = summary.nodeId.empty() ? summary.clusterId : summary.nodeId;
    return Cluster::ProvenFleet::FromBeaconProof(ProvenBy(summary, machine));
}

/// @p summary as a seed answers it: signed by the key of its node over a one-use nonce this side
/// drew, and verified the way `DialledFleetProbe` verifies one -- so its origin is the SOURCE's,
/// stamped by `ProvenFleet::FromSeedAnswer` and never by the caller.
/// @param summary What the seed's node says; its `nodeId` names the key, or its cluster id when it
///        names no node.
/// @param source Which seed source named the seed.
/// @return The proven fleet.
[[nodiscard]] inline Cluster::ProvenFleet ProvenSeed(CompileCacheWire::FleetSummary const& summary,
                                                     Cluster::SeedSource source)
{
    auto const machine = summary.nodeId.empty() ? summary.clusterId : summary.nodeId;
    auto const pair = TestKeyPair(machine);
    auto fresh = IssuedNonceOf(std::byte { 0x5E });
    auto reply = CompileCacheWire::FleetSummaryReply { .summary = summary };
    reply.publicKey = pair.PublicKey();
    reply.signature = SignLabelled(pair, Cluster::FleetSummaryMessage(fresh.wire, summary, pair.PublicKey()));
    auto proven = Cluster::ProvenFleet::FromSeedAnswer(std::move(fresh.held), reply, source);
    REQUIRE(proven.has_value());
    return Unwrap(proven);
}

/// Where every fleet a case names is said to have been created, unless it says otherwise.
inline constexpr std::uint64_t OfficeCreatedAt = 100;

/// The record of a node that minted @p clusterId at @p created and has done nothing since.
/// @param clusterId Its own cluster.
/// @param created When it minted it.
/// @return The record.
[[nodiscard]] inline Cluster::FormationRecord Minted(std::string_view clusterId, std::uint64_t created)
{
    return Cluster::FormationRecord { .mode = Cluster::NodeMode::Solitary,
                                      .own = { .clusterId = std::string { clusterId }, .createdAtUnixSeconds = created },
                                      .joining = std::nullopt,
                                      .fleet = std::nullopt,
                                      .archivePending = std::nullopt,
                                      .rejectedBy = std::nullopt,
                                      .askedJoins = {} };
}

/// What fleet @p target says about itself: established, created at `OfficeCreatedAt`, its leader
/// answering at @p endpoint.
/// @param target The fleet's cluster id.
/// @param endpoint Where its leader answers the `0xFC` port.
/// @return The summary.
[[nodiscard]] inline CompileCacheWire::FleetSummary OfficeSummary(std::string_view target, std::string_view endpoint)
{
    return CompileCacheWire::FleetSummary { .clusterId = std::string { target },
                                            .state = CompileCacheWire::FleetState::Established,
                                            .createdAtUnixSeconds = OfficeCreatedAt,
                                            .leaderId = "n-office",
                                            .leaderNodeEndpoint = std::string { endpoint },
                                            .nodeId = "n-office",
                                            .raftEndpoint = "" };
}

/// The record of a node that minted @p own and asked @p target, at @p endpoint, to take it -- with
/// the memo of that ask, as `RecordJoinTarget` writes one.
/// @param own Its own cluster.
/// @param created When it minted it.
/// @param target The fleet it asked.
/// @param endpoint Where that fleet's leader answers.
/// @return The record.
[[nodiscard]] inline Cluster::FormationRecord Pending(std::string_view own,
                                                      std::uint64_t created,
                                                      std::string_view target,
                                                      std::string_view endpoint)
{
    auto record = Minted(own, created);
    record.mode = Cluster::NodeMode::Pending;
    // The key that proved the summary: its speaker's, as a proof signs it.
    auto const key = TestKeyPair("n-office").PublicKey();
    record.joining =
        Cluster::JoinTarget { .summary = OfficeSummary(target, endpoint), .provenKey = key, .askedAtUnixSeconds = 42 };
    record.askedJoins.push_back(
        Cluster::AskedJoin { .clusterId = std::string { target }, .provenKey = key, .askedAtUnixSeconds = 42 });
    return record;
}

/// A fleet's roster as it admits @p id: @p voter its voter, @p id a learner, each under its own
/// `TestKeyPair`.
/// @param voter The fleet's voter; its host is its id after the `n-` prefix.
/// @param id The admitted machine.
/// @return The roster's bytes.
[[nodiscard]] inline std::vector<std::byte> RosterWith(std::string const& voter, std::string const& id)
{
    auto roster = Cluster::Roster {
        .members = { Cluster::RosterMember { .id = voter,
                                             .raftEndpoint = voter.substr(2) + ":6680",
                                             .seat = Cluster::MemberSeat::Voter,
                                             .publicKey = TestKeyPair(voter).PublicKey() },
                     Cluster::RosterMember { .id = id,
                                             .raftEndpoint = "",
                                             .seat = Cluster::MemberSeat::Learner,
                                             .publicKey = TestKeyPair(id).PublicKey() } },
        .principals = {},
        .revoked = {},
    };
    std::ranges::sort(roster.members, {}, &Cluster::RosterMember::id); // sorted as `ClusterState` sorts
    return Cluster::EncodeRoster(roster);
}

/// The office fleet's roster as it admits @p id: `n-office` -- the member that proved the office's
/// summary -- its voter, and @p id a learner. What `CheckAdmission` accepts for that machine.
/// @param id The admitted machine.
/// @return The roster's bytes.
[[nodiscard]] inline std::vector<std::byte> OfficeRosterWith(std::string const& id)
{
    return RosterWith("n-office", id);
}

/// The record of a node that minted @p own and is a learner of @p fleet, admitted by the office's
/// roster.
/// @param own Its own cluster.
/// @param fleet The fleet it joined.
/// @return The record.
[[nodiscard]] inline Cluster::FormationRecord LearnerIn(std::string_view own, std::string_view fleet)
{
    auto record = Minted(own, 500);
    record.mode = Cluster::NodeMode::Learner;
    record.fleet = Cluster::FleetMembership { .clusterId = std::string { fleet },
                                              .roster = OfficeRosterWith("n-laptop"),
                                              .createdAtUnixSeconds = OfficeCreatedAt };
    return record;
}

/// A formation store in memory: every record saved, in order, and a switch that makes saves fail.
///
/// The record a case reads back is the LAST saved, as a file store's would be; `Saves()` is what a case
/// asserts about ORDER -- that the record was written before the node acted on it.
class InMemoryFormationStore final: public Cluster::IFormationStore
{
  public:
    /// @return The last record saved, or nothing when none was.
    [[nodiscard]] std::expected<std::optional<Cluster::FormationRecord>, ConsensusError> Load() const override
    {
        if (_saves.empty())
            return std::optional<Cluster::FormationRecord> {};
        return std::optional { _saves.back() };
    }

    /// Keep @p record, or refuse with the scripted reason.
    /// @param record The record.
    /// @return Nothing, or the scripted refusal.
    [[nodiscard]] std::expected<void, ConsensusError> Save(Cluster::FormationRecord const& record) override
    {
        if (_failure.has_value())
            return std::unexpected(StorageFailure(*_failure));
        _saves.push_back(record);
        return {};
    }

    /// Make every later save fail with @p reason, as a full disk does.
    /// @param reason What the refusal says.
    void FailSaves(std::string reason)
    {
        _failure = std::move(reason);
    }

    /// @return Every record kept, in order.
    [[nodiscard]] std::vector<Cluster::FormationRecord> const& Saves() const noexcept
    {
        return _saves;
    }

  private:
    std::vector<Cluster::FormationRecord> _saves;
    std::optional<std::string> _failure;
};

} // namespace FastCache::Testing
