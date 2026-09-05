// SPDX-License-Identifier: Apache-2.0
#include "WorkerLease.hpp"

// Its own header FIRST and in a group of its own, so clang-format's alphabetical
// sort inside a group cannot demote it. A translation unit that includes something
// else before its own header stops proving that header is self-contained -- which
// this project requires of every public header -- and the proof is lost silently.
#include "DiscoveryTier.hpp"
#include "NodeMembership.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <string>
#include <utility>

namespace FastCache::Node
{

std::expected<Cc::LeaseValidator, std::string> MakeWorkerLeaseValidator(NodeConfig const& cfg,
                                                                        std::string_view advertise,
                                                                        SocketActivation activation,
                                                                        IWallClock const& clock,
                                                                        Distributed::WorkerLeaseState& lease,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger)
{
    if (cfg.clusterKeyFile.empty())
    {
        // The half `StartupPolicyRejection` cannot decide. It reads `--bind`, which
        // describes nothing under socket activation -- so a keyless node whose unit
        // opened a network port reaches here having passed the table, and must not be
        // allowed to build a validator that refuses nothing.
        //
        // Only the activated case: an ordinary node's `--bind` was already judged, and
        // repeating that judgement here would refuse the loopback fleets this
        // repository's own fixtures run.
        if (activation == SocketActivation::Yes && AdmitsRemotePeers(cfg))
            return std::unexpected { std::string {
                "a socket-activated worker that admits peers on other machines needs "
                "--cluster-key-file: the socket unit chose the address this port answers on, so "
                "--bind describes nothing and cannot show the port is local. Without the key this "
                "node cannot check the lease a client presents, and would compile for anybody who "
                "can reach it" } };

        // Warn rather than Info, and said once at startup rather than per request:
        // the configuration is legitimate for a node no other machine can dial, and
        // an operator who did not intend it has exactly one chance to find out.
        logger.Logf(LogLevel::Warn,
                    "compiling WITHOUT verifying lease signatures: no --cluster-key-file is configured, so a "
                    "grant cannot be checked. The startup rules refuse every configuration in which a machine "
                    "that is not this one could reach the compile verbs -- on --bind and on --listen-node, "
                    "which answers them too -- but no lease is being enforced");
        return Cc::UncheckedLeaseValidator();
    }

    auto key = ReadClusterKey(cfg.clusterKeyFile);
    if (!key.has_value())
        return std::unexpected { key.error() };

    // The cluster is named in the line as well as checked, because "the key
    // verified and the fleet did not" is the failure two sites provisioned from one
    // key file produce, and an operator reading a startup line is who has to notice
    // that this node believes it belongs to a cluster they did not mean (#322).
    logger.Logf(LogLevel::Info,
                "verifying lease signatures against the cluster key, for grants naming {} issued by cluster '{}'",
                advertise,
                cfg.clusterId);
    return Cc::SignedLeaseValidator(*std::move(key), std::string { advertise }, cfg.clusterId, clock, lease, metrics);
}

} // namespace FastCache::Node
