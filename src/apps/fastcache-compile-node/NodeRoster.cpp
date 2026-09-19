// SPDX-License-Identifier: Apache-2.0
#include "NodeRoster.hpp"

// Its own header FIRST and in a group of its own, for `WorkerLease.cpp`'s reason.
#include "NodeMembership.hpp"

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

namespace FastCache::Node
{

std::expected<std::unique_ptr<NodeRoster>, std::string> NodeRoster::Build(NodeConfig const& cfg,
                                                                          WallClockRef wallClock,
                                                                          IMetricsSink& metrics,
                                                                          ILogger& logger)
{
    // A consensus member's roster is the state it applies, whatever else it names.
    if (RunsConsensus(cfg))
        return std::unique_ptr<NodeRoster> { new NodeRoster {
            wallClock, std::make_unique<Distributed::StateLeaseRoster>(), nullptr, nullptr, metrics, logger } };

    // What an earlier run adopted, when this node keeps a state directory. Read BEFORE the
    // anchors are considered: once a roster has been adopted it is the trust root, and the
    // anchors are never read again.
    auto kept = std::optional<Cluster::PersistedRoster> {};
    auto store = std::unique_ptr<Distributed::IRosterStore> {};
    if (!cfg.clusterDir.empty())
    {
        auto const path = cfg.clusterDir / Distributed::RosterFileName;
        auto loaded = Distributed::LoadPersistedRoster(path);
        if (!loaded.has_value())
            return std::unexpected { std::move(loaded).error() };
        kept = *std::move(loaded);
        if (kept.has_value() && cfg.clusterIdExplicit && kept->certificate.clusterId != cfg.clusterId)
            return std::unexpected { std::format(
                "{} holds the roster of cluster '{}', and --cluster-id asserts '{}': this machine was adopted by "
                "another fleet. Move the file aside only if it should now trust its --voter-key anchors instead",
                path.string(),
                kept->certificate.clusterId,
                cfg.clusterId) };
        store = std::make_unique<Distributed::FileRosterStore>(path);
    }

    if (!kept.has_value() && cfg.voterKeys.empty())
    {
        // Nothing to verify a grant against. Legal only where no other machine can present one
        // -- the table refused the rest before any state directory was asked, and this is the
        // same rule answered for a node whose directory turned out to hold nothing.
        if (RunsWorker(cfg) && CompileVerbsReachOtherMachines(cfg))
            return std::unexpected { std::string { RosterlessWorkerRefusal } };
        return std::unique_ptr<NodeRoster> { new NodeRoster { wallClock, nullptr, nullptr, nullptr, metrics, logger } };
    }

    auto asserted = cfg.clusterIdExplicit ? std::optional { cfg.clusterId } : std::nullopt;
    auto trust = std::make_unique<Distributed::RosterTrust>(
        std::move(asserted), cfg.voterKeys, std::move(kept), store.get(), metrics, logger);
    return std::unique_ptr<NodeRoster> { new NodeRoster {
        wallClock, nullptr, std::move(store), std::move(trust), metrics, logger } };
}

NodeRoster::NodeRoster(WallClockRef wallClock,
                       std::unique_ptr<Distributed::StateLeaseRoster> state,
                       std::unique_ptr<Distributed::IRosterStore> store,
                       std::unique_ptr<Distributed::RosterTrust> trust,
                       IMetricsSink& metrics,
                       ILogger& logger):
    _wallClock { wallClock },
    _state { std::move(state) },
    _store { std::move(store) },
    _trust { std::move(trust) },
    _metrics { metrics },
    _logger { logger }
{
}

Distributed::ILeaseRoster const* NodeRoster::Lease() const noexcept
{
    if (_state != nullptr)
        return _state.get();
    return _trust.get();
}

ServerStanding NodeRoster::StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const
{
    auto const* const roster = Lease();
    if (roster == nullptr || roster->Read(_wallClock.Now()).standing == Distributed::RosterStanding::Absent)
        return ServerStanding::Unchecked;

    // A revocation first, whatever id it was revoked under: the key is the fact, and a removed
    // machine claiming a voter's id is still the removed machine.
    auto const keys = roster->KeysOf(serverId);
    if (std::ranges::contains(keys.revoked, serverKey))
        return ServerStanding::Revoked;
    if (keys.live == serverKey)
        return ServerStanding::Voter;
    // A consensus member whose applied state names no voter's key yet has nothing to place a server
    // against -- itself included, when it schedules for itself. Calling that server a stranger made
    // every such node refuse to prove itself to its OWN scheduler until a heartbeat round after the
    // commit that recorded its key, warning at every start. The revocation above is still asked.
    if (_state != nullptr && !_state->HoldsVoterKeys())
        return ServerStanding::Unchecked;
    return ServerStanding::NotVoter;
}

void NodeRoster::Applied(Cluster::ClusterState const& state)
{
    if (_state != nullptr)
        _state->Adopt(state);
}

void NodeRoster::Endorsed(Cluster::RosterEndorsement const& endorsement)
{
    auto encoded = Cluster::EncodeEndorsement(endorsement);
    std::scoped_lock const lock { _endorsementMutex };
    _endorsement = std::move(encoded);
}

std::vector<std::byte> NodeRoster::Endorsement() const
{
    std::scoped_lock const lock { _endorsementMutex };
    return _endorsement;
}

void NodeRoster::Offered(std::span<std::byte const> certified)
{
    // A consensus member has its state, and a node with no trust root could judge nothing: a
    // roster handed to either is ignored rather than adopted on the scheduler's word.
    if (_trust == nullptr || certified.empty())
        return;

    auto decoded = Cluster::DecodeCertifiedRoster(certified);
    if (!decoded.has_value())
    {
        // Not certified, since nothing in it could be checked: counted as one, and said once.
        _metrics.Increment(IMetricsSink::Counter::WorkerRostersRefusedUncertified);
        std::scoped_lock const lock { _endorsementMutex };
        if (!_warnedMalformed)
        {
            _warnedMalformed = true;
            _logger.Logf(LogLevel::Warn,
                         "roster: a scheduler answered with a roster this build cannot read ({}); keeping the one this "
                         "node holds (every such answer is counted; this line is not repeated)",
                         decoded.error().context);
        }
        return;
    }
    std::ignore = _trust->Offer(*decoded, _wallClock.Now());
}

bool NodeRoster::Wanting() const
{
    return _trust != nullptr && _trust->Read(_wallClock.Now()).standing != Distributed::RosterStanding::Current;
}

std::optional<Distributed::RosterSummary> NodeRoster::Summary() const
{
    if (_state != nullptr)
        return _state->Summary();
    if (_trust != nullptr)
        return _trust->Summary();
    return std::nullopt;
}

std::optional<std::uint64_t> NodeRoster::ExpiresInSeconds() const
{
    if (_trust == nullptr)
        return std::nullopt;
    auto const reading = _trust->Read(_wallClock.Now());
    if (!reading.certifiedUntil.has_value())
        return std::nullopt;
    auto const now = _wallClock.Now();
    if (*reading.certifiedUntil <= now)
        return std::uint64_t { 0 };
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(*reading.certifiedUntil - now).count());
}

} // namespace FastCache::Node
