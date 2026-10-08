// SPDX-License-Identifier: Apache-2.0
#include "WorkerLease.hpp"

// Its own header FIRST and in a group of its own, so clang-format's alphabetical
// sort inside a group cannot demote it. A translation unit that includes something
// else before its own header stops proving that header is self-contained -- which
// this project requires of every public header -- and the proof is lost silently.
#include "NodeMembership.hpp"

#include <FastCache/Core/HostPort.hpp>

#include <format>
#include <string>
#include <utility>

namespace FastCache::Node
{

std::expected<Cc::LeaseValidator, std::string> MakeWorkerLeaseValidator(NodeConfig const& cfg,
                                                                        Distributed::ILeaseRoster const* roster,
                                                                        Cc::IAdvertisedEndpointSource const& advertise,
                                                                        std::span<std::byte const> identityKey,
                                                                        SocketActivation activation,
                                                                        core::platform::WallClockRef clock,
                                                                        Distributed::WorkerLeaseState& lease,
                                                                        IMetricsSink& metrics,
                                                                        ILogger& logger,
                                                                        LeaseCheckInForce& inForce)
{
    if (roster == nullptr)
    {
        // The half `StartupPolicyRejection` cannot be relied on to decide. A START now judges the
        // handed-over socket's adopted bind (`AdoptActivatedBind`), but the table is also asked of
        // the TYPED `--listen-node` an `--install-service` registration replays, which under
        // activation describes no socket at all -- and the rule this guard keeps is about a
        // listener this process did not open. So a node with no roster whose unit opened a
        // network port must not be allowed to build a validator that refuses nothing, whatever
        // the table concluded.
        //
        // Only the activated case: an ordinary node's `--bind` was already judged, and
        // repeating that judgement here would refuse the loopback fleets this
        // repository's own fixtures run.
        //
        // `Absent`, because that is what `roster == nullptr` states: with no roster no
        // proof or ticket admits anybody, so only `--fleet-open` or a fleet the formation
        // record puts this node in widens it.
        if (activation == SocketActivation::Yes && AdmitsRemotePeers(cfg, RosterPresence::Absent))
            return std::unexpected { std::string {
                "a socket-activated worker that admits peers on other machines needs a roster to verify "
                "leases against, and the only roster is the state consensus applies -- so run consensus "
                "(--listen-raft) or admit only this machine: the socket unit chose the address this port answers "
                "on, so no flag of this node can show the port is local. Without one this node cannot "
                "check the lease a client presents, and would compile for anybody who can reach it" } };

        // Warn rather than Info, and said once at startup rather than per request:
        // the configuration is legitimate for a node no other machine can dial, and
        // an operator who did not intend it has exactly one chance to find out.
        logger.Logf(LogLevel::Warn,
                    "compiling WITHOUT verifying lease signatures: this node runs no consensus and so keeps no "
                    "roster, and a grant cannot be checked. The startup rules refuse every "
                    "configuration in which a machine that is not this one could reach the compile verbs -- on "
                    "--bind and on --listen-node, which answers them too -- but no lease is being enforced");
        inForce.Record(BuiltLeaseCheck::Unchecked);
        return Cc::UncheckedLeaseValidator();
    }

    // The fleet is not named here. It is learned from the REGISTER reply and read per request
    // out of `lease.fleet` (#401), so this line can only say what this node was configured to
    // REACH -- and the identity it ends up pinned to is reported when it registers, which is
    // the moment that fact first exists.
    logger.Logf(LogLevel::Info,
                "verifying lease signatures against the state this node's consensus applies, for grants naming {}; "
                "the fleet is adopted from the scheduler's registration reply",
                // What is advertised NOW, which at this moment is what the process
                // started with -- the seam's value can move later, and this line is a
                // statement about startup. The move itself is announced where it
                // happens (`EndpointResolver`), so no reader has to infer it
                // from a startup line that was true when it was printed.
                advertise.Current());
    inForce.Record(BuiltLeaseCheck::Signed);
    return Cc::SignedLeaseValidator(*roster, advertise, identityKey, clock, lease, metrics);
}

bool ReloadWidensUncheckedWorker(NodeConfig const& previous, NodeConfig const& candidate, BuiltLeaseCheck built)
{
    // `Absent` on both sides: the unchecked validator is built only where no roster is held, so no
    // proof or ticket admits anybody and only `--fleet-open` (or a formation change) can widen.
    return built == BuiltLeaseCheck::Unchecked && AdmitsRemotePeers(candidate, RosterPresence::Absent)
           && !AdmitsRemotePeers(previous, RosterPresence::Absent);
}

std::function<std::expected<void, ConfigError>(NodeConfig const&, NodeConfig const&)> ReloadCheckWith(
    LeaseCheckInForce const& inForce)
{
    return [&inForce](NodeConfig const& previous, NodeConfig const& candidate) -> std::expected<void, ConfigError> {
        if (auto judged = ValidateNodeReloadable(previous, candidate); !judged.has_value())
            return judged;
        if (ReloadWidensUncheckedWorker(previous, candidate, inForce.Current()))
            return std::unexpected(
                ConfigError { .code = ConfigErrorCode::ParseError,
                              .source = {},
                              .line = 0,
                              .field = {},
                              .context = std::format("not applied: {}", ReloadWidensUncheckedWorkerRefusal) });
        return {};
    };
}

} // namespace FastCache::Node
