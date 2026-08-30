// SPDX-License-Identifier: Apache-2.0
#include "DiscoveryTier.hpp"
#include "NodeIoLoop.hpp"
#include "SchedulerTier.hpp"

#include <FastCache/Cluster/ClusterIdentity.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <filesystem>

#include <cstddef>
#include <utility>
#include <vector>

namespace FastCache::Node
{

SchedulerTier::SchedulerTier(Distributed::IMembershipOracle const& membership,
                             IClock& clock,
                             IWallClock const& wallClock,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             std::span<std::byte const> signingKey):
    _service { clock, wallClock, metrics, logger, signingKey },
    _protocol { _service },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    _responder { _protocol, membership }
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

namespace
{
    /// Where this node keeps its fleet identity, or empty when it keeps nothing.
    ///
    /// The same chain `HistoryPaths` walks, and for the same reason: the consensus
    /// directory when there is one, the cache directory otherwise. A node with
    /// neither is genuinely stateless on disk.
    /// @param cfg What this node was told to be.
    /// @return The file, or empty when there is nowhere durable to put it.
    [[nodiscard]] std::filesystem::path FleetIdentityPath(NodeConfig const& cfg)
    {
        if (!cfg.clusterDir.empty())
            return cfg.clusterDir / Cluster::ClusterIdFileName;
        if (!cfg.cacheDir.empty())
            return cfg.cacheDir / Cluster::ClusterIdFileName;
        return {};
    }

    /// Read this node's fleet identity, minting one the first time.
    ///
    /// A node with nowhere durable to write gets a fresh identity per process and is
    /// told so, once. That is the honest degradation rather than the safe-looking
    /// one: its workers pin the first identity they see and refuse every later one,
    /// so this node's own restarts will be refused until those workers restart too.
    /// Silence there would present as a fleet that stopped distributing after a
    /// routine restart, with a security-flavoured error and nothing naming the cause.
    /// @param cfg What this node was told to be.
    /// @param logger Where that warning goes.
    /// @return The identity, or why one could not be established.
    [[nodiscard]] std::expected<std::string, std::string> EstablishFleetIdentity(NodeConfig const& cfg, ILogger& logger)
    {
        SystemRandomSource random;
        auto const path = FleetIdentityPath(cfg);
        if (!path.empty())
            return Cluster::LoadOrMintClusterId(path, random);

        logger.Logf(LogLevel::Warn,
                    "this node has no durable directory (--cluster-dir or --cache-disk), so its fleet identity is "
                    "minted fresh at every start; workers that have served this fleet will refuse its grants after a "
                    "restart until they restart too");
        return Cluster::MintClusterId(random);
    }
} // namespace

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

    auto tier =
        std::unique_ptr<SchedulerTier> { new SchedulerTier { membership, clock, wallClock, metrics, logger, signingKey } };

    // Which FLEET this is, signed into every grant so two clusters provisioned from
    // one key file do not honour each other's work (#322). Only where something is
    // signed at all: with no key there is no grant to bind, and minting an identity
    // for a node that cannot use it would create a file to explain and nothing else.
    if (!signingKey.empty())
    {
        auto identity = EstablishFleetIdentity(cfg, logger);
        if (!identity.has_value())
            return std::unexpected { identity.error() };
        tier->_service.SetClusterId(*identity);
    }

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
