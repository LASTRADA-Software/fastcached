// SPDX-License-Identifier: Apache-2.0
#include "WorkerLease.hpp"

// Its own header FIRST and in a group of its own, so clang-format's alphabetical
// sort inside a group cannot demote it. A translation unit that includes something
// else before its own header stops proving that header is self-contained -- which
// this project requires of every public header -- and the proof is lost silently.
#include "NodeMembership.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <string>
#include <utility>

namespace FastCache::Node
{

std::expected<Cc::LeaseValidator, std::string> MakeWorkerLeaseValidator(NodeConfig const& cfg,
                                                                        Distributed::ILeaseRoster const* roster,
                                                                        Cc::IAdvertisedEndpointSource const& advertise,
                                                                        SocketActivation activation,
                                                                        core::platform::WallClockRef clock,
                                                                        Distributed::WorkerLeaseState& lease,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger)
{
    if (roster == nullptr)
    {
        // The half `StartupPolicyRejection` cannot decide. It reads `--bind`, which
        // describes nothing under socket activation -- so a node with no roster whose unit
        // opened a network port reaches here having passed the table, and must not be
        // allowed to build a validator that refuses nothing.
        //
        // Only the activated case: an ordinary node's `--bind` was already judged, and
        // repeating that judgement here would refuse the loopback fleets this
        // repository's own fixtures run.
        if (activation == SocketActivation::Yes && AdmitsRemotePeers(cfg))
            return std::unexpected { std::string {
                "a socket-activated worker that admits peers on other machines needs a roster to verify "
                "leases against -- name the cluster's voters with --voter-key, or run consensus: the socket "
                "unit chose the address this port answers on, so --bind describes nothing and cannot show the "
                "port is local. Without one this node cannot check the lease a client presents, and would "
                "compile for anybody who can reach it" } };

        // Warn rather than Info, and said once at startup rather than per request:
        // the configuration is legitimate for a node no other machine can dial, and
        // an operator who did not intend it has exactly one chance to find out.
        logger.Logf(LogLevel::Warn,
                    "compiling WITHOUT verifying lease signatures: this node runs no consensus, names no "
                    "--voter-key and keeps no roster, so a grant cannot be checked. The startup rules refuse every "
                    "configuration in which a machine that is not this one could reach the compile verbs -- on "
                    "--bind and on --listen-node, which answers them too -- but no lease is being enforced");
        return Cc::UncheckedLeaseValidator();
    }

    // The fleet is not named here. It is learned from the REGISTER reply and read per request
    // out of `lease.fleet` (#401), so this line can only say what this node was configured to
    // REACH -- and the identity it ends up pinned to is reported when it registers, which is
    // the moment that fact first exists.
    logger.Logf(LogLevel::Info,
                "verifying lease signatures against {}, for grants naming {}; the fleet is adopted from the "
                "scheduler's registration reply",
                RunsConsensus(cfg) ? "the roster this node applies" : "the roster the cluster's voters certify",
                // What is advertised NOW, which at this moment is what the process
                // started with -- the seam's value can move later, and this line is a
                // statement about startup. The move itself is announced where it
                // happens (`AdvertisedEndpointChange`), so no reader has to infer it
                // from a startup line that was true when it was printed.
                advertise.Current());
    return Cc::SignedLeaseValidator(*roster, advertise, clock, lease, metrics);
}

} // namespace FastCache::Node
