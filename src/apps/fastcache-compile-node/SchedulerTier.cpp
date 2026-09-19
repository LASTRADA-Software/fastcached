// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "NodeIoLoop.hpp"
#include "SchedulerTier.hpp"

#include <cstddef>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Node
{

SchedulerTier::SchedulerTier(Distributed::IMembershipOracle const& membership,
                             IClock& clock,
                             WallClockRef wallClock,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             std::string signerId,
                             Ed25519KeyPair identityKey,
                             std::string_view clusterId,
                             std::shared_ptr<AuthPolicy const> policy):
    _signer { std::move(signerId), std::move(identityKey) },
    _service { clock, wallClock, metrics, logger, _signer, clusterId },
    _protocol { _service, metrics },
    // The oracle is the NODE's, not this tier's: the cache surface consults the same
    // object, and a node that answered "is this peer one of ours" differently at its
    // two surfaces would admit a peer to the fleet and refuse it the objects that
    // fleet produced. It also outlives this tier, which is what lets a node serve a
    // cache with no scheduler at all.
    // Kept as well as handed on, because the enrollment surface requires the SAME
    // credential rather than one of its own: `AUTH` is a `Session` verb and the merged
    // listener routes it to the scheduler, so a second policy object would be one this
    // node never checks anything against. A `shared_ptr` copy, so neither holder owns
    // the other's lifetime.
    _policy { policy },
    _responder { _protocol, membership, metrics, std::move(policy) }
{
    // No standalone leadership any more (#178). Every scheduler runs consensus -- a lone one
    // is a cluster of one -- so the service keeps its own `Undecided` default until the
    // consensus tier publishes a role and a term, which `Gate()` answers with `NotLeader`
    // meanwhile. "Leader at term 0" is not a weaker answer than "leader at term N"; it is a
    // different and wrong one (#613), and there is no longer a node for whom it is right.
}

std::expected<std::unique_ptr<SchedulerTier>, std::string> SchedulerTier::Start(
    NodeConfig const& cfg,
    Distributed::IMembershipOracle const& membership,
    IClock& clock,
    WallClockRef wallClock,
    IMetricsSink& metrics,
    ILogger& logger,
    std::optional<Ed25519KeyPair> const& identityKey)
{
    // The key every grant is signed with is this node's OWN (#178), resolved out of its state
    // directory before any tier exists. A scheduler runs consensus and a consensus node always
    // holds one, so this is the answer to a caller that did not -- never a fallback to
    // unsigned grants, which no longer exist.
    if (!identityKey.has_value())
        return std::unexpected { std::string { SchedulerNeedsIdentityKeyRefusal } };

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
        membership, clock, wallClock, metrics, logger, cfg.nodeId, *identityKey, cfg.clusterId, std::move(policy) } };

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

void SchedulerTier::Endorse(Cluster::RosterEndorsement const& endorsement)
{
    std::scoped_lock const lock { _ownEndorsementMutex };
    _ownEndorsement = endorsement;
    std::ignore = _service.AcceptEndorsement(endorsement);
}

void SchedulerTier::Administer(Distributed::IClusterAdmin& admin)
{
    std::scoped_lock const lock { _ownEndorsementMutex };
    _service.AdministerWith(admin);
    if (_ownEndorsement.has_value())
        std::ignore = _service.AcceptEndorsement(*_ownEndorsement);
}

} // namespace FastCache::Node
