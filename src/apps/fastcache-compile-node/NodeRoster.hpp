// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"
#include "NodeProofClient.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/RosterCertificate.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/RosterStore.hpp>
#include <FastCache/Distributed/RosterTrust.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <core/platform/Clock.hpp>

/// @file NodeRoster.hpp
/// The roster a node verifies lease grants against, and the half of NODE-ANNOUNCE that feeds
/// it (#178).
///
/// Every grant is signed by the voter that issued it, with its own identity key, so a worker
/// asks "is this signer one of the cluster's voters, unrevoked" -- and the answer comes from a
/// different place depending on what the node is:
///
/// - a CONSENSUS member reads the state it applied (`Distributed::StateLeaseRoster`), which
///   needs no certificate and never expires;
/// - any other node holds the roster a strict majority of the voters it trusts endorsed
///   (`Distributed::RosterTrust`), rooted in `--voter-key` or in the roster it kept in its state
///   directory, and refreshed by NODE-ANNOUNCE's reply;
/// - a node no other machine can reach holds neither, and checks no grant -- the rule the
///   startup table and `CompileVerbsReachOtherMachines` hold it to.
namespace FastCache::Node
{

/// What a presence round says about the roster, and what it does with the one handed back.
///
/// The seam `AnnounceMachineOnce` reaches it through, so a case can drive a round without a
/// state directory or a consensus tier behind it.
class IPresenceRoster
{
  public:
    IPresenceRoster() = default;
    IPresenceRoster(IPresenceRoster const&) = delete;
    IPresenceRoster(IPresenceRoster&&) = delete;
    IPresenceRoster& operator=(IPresenceRoster const&) = delete;
    IPresenceRoster& operator=(IPresenceRoster&&) = delete;
    virtual ~IPresenceRoster() = default;

    /// @return This node's latest endorsement, encoded for NODE-ANNOUNCE; empty when it signs
    ///         none -- it is no voter, or holds no key the state records.
    [[nodiscard]] virtual std::vector<std::byte> Endorsement() const = 0;

    /// Take what a scheduler handed back.
    /// @param certified NODE-ANNOUNCE's reply: an encoded certified roster, or empty when the
    ///        scheduler has none yet.
    virtual void Offered(std::span<std::byte const> certified) = 0;

    /// @return Whether this node holds no roster it could verify a grant against right now,
    ///         and so should ask again sooner than an ordinary round.
    [[nodiscard]] virtual bool Wanting() const = 0;
};

/// How often a node that holds no current roster asks again, instead of waiting a whole
/// `NodeAnnounceInterval`.
///
/// Short, because every grant is refused until a roster arrives -- a worker that has just
/// started, or one whose roster lapsed, is a worker compiling nothing -- and bounded below by
/// what a scheduler can bear: one announcement per wanting node every few seconds.
inline constexpr std::chrono::seconds RosterWantingInterval { 2 };

/// The roster this node verifies grants against, as one object (#178).
class NodeRoster final: public IPresenceRoster, public IServerTrust
{
  public:
    /// Build the roster this configuration calls for.
    ///
    /// Refuses a kept roster that cannot be used -- it may be the only thing standing between
    /// this worker and a voter the cluster has revoked since -- and one naming a fleet other
    /// than `--cluster-id` asserts. And refuses a worker other machines can reach that holds no
    /// roster and names no `--voter-key`, which is where `RosterlessWorkerRefusal` is answered:
    /// only the state directory knows whether it holds a roster.
    /// @param cfg The parsed configuration.
    /// @param wallClock What "now" is when an offered roster is judged. Borrowed.
    /// @param metrics Where a refused roster is counted. Borrowed.
    /// @param logger Where an adoption and a refusal are said. Borrowed.
    /// @return The roster, or why the node must not start.
    [[nodiscard]] static std::expected<std::unique_ptr<NodeRoster>, std::string> Build(
        NodeConfig const& cfg, core::platform::WallClockRef wallClock, IMetricsSink& metrics, ILogger& logger);

    /// @return What a grant is verified against, or null when this node verifies none.
    [[nodiscard]] Distributed::ILeaseRoster const* Lease() const noexcept;

    /// @copydoc IServerTrust::StandingOf
    ///
    /// The roster a GRANT is verified against is the roster a server is (#178): a machine may
    /// prove itself only to a scheduler whose grants it would accept, so both questions read the
    /// one `Lease()`. A roster not yet held -- a fresh worker, a node verifying no grants -- checks
    /// nothing and says so, and the seal is what still protects the connection.
    [[nodiscard]] ServerStanding StandingOf(std::string_view serverId, Ed25519PublicKey const& serverKey) const override;

    /// Adopt what the cluster now says. A consensus member's roster only; a no-op otherwise.
    /// @param state The replicated state.
    void Applied(Cluster::ClusterState const& state);

    /// Remember the endorsement this node just signed, for the next announcement.
    /// @param endorsement What the consensus tier signed.
    void Endorsed(Cluster::RosterEndorsement const& endorsement);

    [[nodiscard]] std::vector<std::byte> Endorsement() const override;

    void Offered(std::span<std::byte const> certified) override;

    [[nodiscard]] bool Wanting() const override;

    /// @return The roster held, summarised for `--node-status`; nothing on a node that holds
    ///         none. `certifiedUntil` is absent on a consensus member, whose roster is its state.
    [[nodiscard]] std::optional<Distributed::RosterSummary> Summary() const;

    /// @return Whole seconds until the held roster's certification lapses, 0 once it has; nothing
    ///         on a node whose roster has no certificate to lapse -- a consensus member, or one
    ///         that holds none.
    [[nodiscard]] std::optional<std::uint64_t> ExpiresInSeconds() const;

    NodeRoster(NodeRoster const&) = delete;
    NodeRoster(NodeRoster&&) = delete;
    NodeRoster& operator=(NodeRoster const&) = delete;
    NodeRoster& operator=(NodeRoster&&) = delete;
    ~NodeRoster() override = default;

  private:
    NodeRoster(core::platform::WallClockRef wallClock,
               std::unique_ptr<Distributed::StateLeaseRoster> state,
               std::unique_ptr<Distributed::IRosterStore> store,
               std::unique_ptr<Distributed::RosterTrust> trust,
               IMetricsSink& metrics,
               ILogger& logger);

    core::platform::WallClockRef _wallClock;

    /// A consensus member's roster; null on any other node.
    std::unique_ptr<Distributed::StateLeaseRoster> _state;

    /// Where `_trust` keeps what it adopts; null without a state directory. Declared before
    /// `_trust`, which borrows it.
    std::unique_ptr<Distributed::IRosterStore> _store;

    /// A node without consensus: the roster its voters certified; null when it holds none and
    /// trusts no anchors.
    std::unique_ptr<Distributed::RosterTrust> _trust;

    IMetricsSink& _metrics;
    ILogger& _logger;

    /// The latest endorsement this node signed, encoded. Guarded by `_endorsementMutex`: the
    /// reconciler writes it and the presence loop reads it.
    mutable std::mutex _endorsementMutex;
    std::vector<std::byte> _endorsement;

    /// Whether a malformed reply has been said once already; guarded by `_endorsementMutex`.
    bool _warnedMalformed { false };
};

} // namespace FastCache::Node
