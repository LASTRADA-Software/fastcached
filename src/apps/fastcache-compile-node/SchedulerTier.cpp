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
                             std::shared_ptr<AuthPolicy const> policy):
    _service { clock, wallClock, metrics, logger, signingKey },
    _protocol { _service },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    _responder { _protocol, membership, metrics, std::move(policy) }
{
    // Standalone leadership, which is now a DEFAULT rather than a placeholder. A node
    // with no `--node-id` runs no consensus and is the only scheduler there is: it
    // hands out its own machine's slots and nobody else's, which is exactly right for
    // the one-machine deployment and is what most people run.
    //
    // With consensus configured, `ConsensusTier` calls `SetRole` the moment the
    // driver decides anything, and this initial value is superseded before the first
    // lease. It is deliberately not `Undecided`: a node that refused every verb until
    // an election completed would be strictly worse than what it replaces at exactly
    // the moment somebody is watching it start.
    _service.SetRole(Distributed::SchedulerRole::Leader, {});
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeIoLoop& io,
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

    auto tier = std::unique_ptr<SchedulerTier> { new SchedulerTier {
        membership, clock, wallClock, metrics, logger, signingKey, std::move(policy) } };

    // The surface, not an address. A bare port binds the WILDCARD here, the opposite
    // of the cache's loopback, and that asymmetry is now a column of this surface's
    // row rather than an argument this call site chooses -- so the address bound
    // here, the one an install-time refusal judges and the one `--print-surfaces`
    // prints are one computation instead of three that agree today.
    auto started = FrameEndpoint::Start(io, NodeSurface::Scheduler, cfg, tier->_responder, logger);
    if (!started.has_value())
        return std::unexpected { started.error() };

    tier->_endpoint = std::move(*started);
    // The phrase is `AdmissionSummary`'s rather than this tier's, because the policy
    // is the NODE's: the worker's own ready line reports the identical fact, and two
    // surfaces spelling one policy differently is how an operator comes to believe
    // their compile port is configured because their scheduler said so (#235).
    logger.Logf(LogLevel::Info, "scheduling for the fleet on {} ({})", tier->BoundEndpoint(), AdmissionSummary(cfg));
    return tier;
}

} // namespace FastCache::Node
