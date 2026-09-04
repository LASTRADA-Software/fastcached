// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "DiscoveryTier.hpp"
#include "NodeIoLoop.hpp"
#include "SchedulerTier.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Node
{

SchedulerTier::SchedulerTier(Distributed::IMembershipOracle const& membership,
                             IClock& clock,
                             IWallClock const& wallClock,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             std::span<std::byte const> signingKey,
                             std::string_view clusterId,
                             std::shared_ptr<AuthPolicy const> policy,
                             LeadsAlone leadsAlone):
    _service { clock, wallClock, metrics, logger, signingKey, clusterId },
    _protocol { _service, metrics },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    _responder { _protocol, membership, metrics, std::move(policy) }
{
    // **Standalone leadership, and ONLY for a node that leads alone** (#613).
    //
    // A node with no `--node-id` runs no consensus and is the only scheduler there is:
    // it hands out its own machine's slots and nobody else's, which is exactly right
    // for the one-machine deployment and is what most people run. Nothing will ever
    // call `SetRole` on it, so leaving it `Undecided` would refuse every verb forever
    // -- and the argument that a node refusing until an election completes is "worse
    // than what it replaces at exactly the moment somebody is watching it start" is
    // right for THIS node, where no election is coming.
    //
    // Term ZERO is an answer rather than a placeholder here: there is no term to be
    // in. Every grant it mints names 0, and the only verifier that could compare terms
    // is one told what is current -- which nothing tells a lone node, because nothing
    // elects it (#322).
    //
    // **It used to be published unconditionally, and that is the defect.** With
    // consensus configured the comment claimed this value "is superseded before the
    // first lease", and it is not: `ConsensusTier` publishes a role from the driver's
    // callback, which cannot run until the reactor starts and then only once an
    // election completes. Until then the surface answered `Lease` as a leader nobody
    // had elected, minting grants at term 0 that any worker which has learned a higher
    // term refuses. "Leader at term 0" is not a weaker answer than "leader at term N";
    // it is a different and wrong one, and a surface must not answer a question whose
    // input it does not have -- the same rule the node already follows between bound
    // and surveyed (#365).
    //
    // A clustered node therefore keeps the service's own `Undecided` default, which
    // `Gate()` already answers with `NotLeader` and the leader's endpoint. That is not
    // a new refusal path: it is the one an election in progress has always produced,
    // and a client answers it by compiling locally. One local compile during a startup
    // election is the cost; the alternative is a grant the fleet will refuse.
    if (leadsAlone == LeadsAlone::Yes)
        _service.SetRole(Distributed::SchedulerRole::Leader, {}, Distributed::StandaloneSchedulerTerm);
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    IWallClock const& wallClock,
    IMetricsSink& metrics,
    ILogger& logger)
{
    // The key a lease grant is signed with, and the same file discovery proves the
    // cluster's identity from -- read again here rather than passed down, because the
    // scheduler is built before discovery is and may be the only one of the two an
    // operator asked for. Reading a small file twice at startup is not a cost worth a
    // dependency between two tiers that otherwise have none.
    //
    // Absent is legal and means unsigned grants; unreadable is not, and is fatal for
    // the reason the header states.
    std::vector<std::byte> signingKey;
    if (!cfg.clusterKeyFile.empty())
    {
        auto key = ReadClusterKey(cfg.clusterKeyFile);
        if (!key.has_value())
            return std::unexpected { key.error() };
        signingKey = std::move(*key);
    }

    // The credential this surface REQUIRES, which is the inbound half of
    // `--requirepass` (#289). Absent is legal and means membership is the only gate;
    // unreadable is fatal, for the reason the key file is -- an operator who named a
    // token file and got an unauthenticated scheduler has a port that looks guarded.
    std::shared_ptr<AuthPolicy const> policy;
    if (!cfg.schedulerTokenFile.empty())
    {
        auto secret = ReadSecretFile(cfg.schedulerTokenFile);
        if (!secret.has_value())
            return std::unexpected { std::format("--scheduler-token-file {}", secret.error()) };
        // No username: every in-tree client presents the `requirepass` form, and
        // `CheckCredential` matches on the secret alone when none is given.
        policy = std::make_shared<AuthPolicy const>(std::string {}, std::move(*secret));
    }

    // Asked of the SHARED predicate, so this tier and `StartConsensusOrExplain` cannot
    // disagree about whether a role is coming (#613).
    auto const leadsAlone = RunsConsensus(cfg) ? LeadsAlone::No : LeadsAlone::Yes;

    auto tier = std::unique_ptr<SchedulerTier> { new SchedulerTier {
        membership, clock, wallClock, metrics, logger, signingKey, cfg.clusterId, std::move(policy), leadsAlone } };

    // No address in this line since #290: the scheduler verbs are answered on the
    // node's one 0xFC listener, and that listener names itself when it binds.
    //
    // The phrase is `AdmissionSummary`'s rather than this tier's, because the policy
    // is the NODE's: the worker's own ready line reports the identical fact, and two
    // surfaces spelling one policy differently is how an operator comes to believe
    // their compile port is configured because their scheduler said so (#235).
    logger.Logf(LogLevel::Info, "scheduling for the fleet ({})", AdmissionSummary(cfg));
    return tier;
}

} // namespace FastCache::Node
