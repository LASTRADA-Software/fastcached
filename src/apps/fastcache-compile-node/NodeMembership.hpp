// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Node
{

/// This node's one answer to "who is this caller to us".
///
/// Owns every oracle and hands out whichever the operator chose, so that **every**
/// surface asks the same object. That is the whole point of the type existing rather
/// than each tier building its own: the scheduler decides who may spend the fleet's
/// CPU and the cache decides who may read this machine's objects, and a node that
/// answered those two questions differently for one peer would admit it to the fleet
/// and refuse it the objects that fleet produced — or, worse, the reverse.
///
/// It also outlives both tiers by construction, which matters because a node may run
/// a cache surface with no scheduler at all: the oracle used to live inside
/// `SchedulerTier`, which made the cache's access policy depend on whether this node
/// happened to be scheduling.
///
/// ## Two lists, because there are two questions
///
/// `--fleet-member` says who may spend this node's CPU and read its cache tier, and
/// that includes machines which are not cluster peers and never will be: a
/// developer's laptop, a CI runner, anything running `fastcache-cc` against the
/// fleet. The cluster's agreed member set says who is in the cluster. Answering both
/// with one list meant the first replicated membership commit discarded everything an
/// operator had listed, and agreeing something is routine -- a node joining, a node
/// being forgotten, a settings change (#251).
///
/// So this owns one `ClusterMembership` per question and composes them, rather than
/// letting either publisher speak for the other. Each is still replaced wholesale by
/// whoever owns it, which is right: a publisher holds the whole truth about *its*
/// question.

/// Whether this configuration admits a machine that is not this one.
///
/// The same three routes `NodeMembership`'s constructor composes, asked of the
/// configuration instead of a caller, and it lives HERE for that reason: when the
/// fourth route lands -- the rulebook says it will, and that it will be a credential
/// rather than a host list -- it is added a few lines below, and a reader adding it
/// has this function in front of them. Left in `NodeConfig.cpp` it was a second
/// reader of the same policy with nothing pointing at it, and a new route would
/// silently stop the startup rule firing: an open compile port, every refusal counter
/// at zero, and a fleet green from both ends.
///
/// It cannot be answered by asking the oracle, which is why it is a separate function
/// rather than a method. `Oracle()` answers "is this caller admitted" in the present
/// tense; this asks whether the admitted set will EVER contain another machine, and
/// `_cluster` is empty at construction by design. The `raftJoin` / `raftPeers` half
/// predicts what consensus will later `Publish()` into this object, which no runtime
/// query can see.
///
/// Only the host is looked at, matching what the oracle itself compares: a peer
/// arrives from an ephemeral source port, so a port was never something a connection
/// could be matched on.
/// @param cfg The parsed configuration.
/// @return Whether the policy admits anything but this machine.
[[nodiscard]] inline bool AdmitsRemotePeers(NodeConfig const& cfg)
{
    auto const remote = [](std::string_view endpoint) {
        return !IsLoopbackHost(HostOfEndpoint(endpoint));
    };

    // `--fleet-open` first: it admits every machine there is, and says so as a flag
    // rather than as an empty list, so nothing here has to infer it.
    if (cfg.fleetOpen)
        return true;
    if (std::ranges::any_of(cfg.fleetMembers, remote))
        return true;

    // A node waiting to be admitted has an EMPTY member set by construction and is
    // about to be handed one -- so the absence of peers here is the strongest signal
    // that remote ones are coming, not the weakest.
    if (cfg.raftJoin)
        return true;
    return std::ranges::any_of(cfg.raftPeers,
                               [&](Cluster::ClusterMember const& member) { return remote(member.raftEndpoint); });
}

/// This node's admission policy, as the seam every surface holds.
///
/// **It IS the oracle rather than handing one out, and that is what makes
/// `--fleet-open` live** ([#405](https://github.com/LASTRADA-Software/fastcached/issues/405)).
/// `Oracle()` used to return one of two owned objects chosen by a `bool` read once,
/// and the surfaces bind that reference for their lifetime -- so flipping the flag on
/// a running node would have changed a member nobody reads again. Answering `Classify`
/// here means the choice is made per request, at the surface, with no call site
/// changed and nothing to remember.
class NodeMembership final: public Distributed::IMembershipOracle
{
  public:
    /// @param cfg The parsed configuration.
    /// @param logger Where the keyless-widening refusal is reported, once.
    NodeMembership(NodeConfig const& cfg, ILogger& logger):
        _open {},
        _listed { cfg.fleetMembers },
        _cluster {},
        // Pointers into this object's own members, which is safe because the type is
        // neither copyable nor movable and the composite is declared after both.
        _admitted { { &_listed, &_cluster } },
        _logger { logger },
        // FIXED for this process. `--cluster-key-file` carries `.same` and no
        // `.reloadable`, so it cannot change under a running node -- which is what
        // lets the widening guard below read it once rather than re-ask per commit.
        _keyless { cfg.clusterKeyFile.empty() },
        _flagOpen { cfg.fleetOpen },
        _isOpen { cfg.fleetOpen }
    {
    }

    NodeMembership(NodeMembership const&) = delete;
    NodeMembership& operator=(NodeMembership const&) = delete;
    NodeMembership(NodeMembership&&) = delete;
    NodeMembership& operator=(NodeMembership&&) = delete;
    ~NodeMembership() override = default;

    /// Adopt what an accepted reload says about who this node admits.
    ///
    /// **The removal direction is what this exists for.** A member ADDED to the file
    /// and not yet admitted fails closed -- a machine is refused until somebody
    /// restarts the node, which is annoying, self-healing and visible. A member
    /// REMOVED from the file and still admitted fails OPEN: a machine the operator has
    /// just revoked keeps being served, and nothing reports it, because admission
    /// succeeding is the ordinary case. Only one of those is worth a live path, and it
    /// is the one a test naturally skips.
    ///
    /// **`--fleet-open` is settled BEFORE the list, and the order is not arbitrary.**
    /// Each half is individually atomic, so no request ever sees a half-written set;
    /// what the order decides is which stale half a request between them may read.
    /// Settling the flag first means a NARROWING reload briefly answers from the new
    /// flag and the old list -- closed, since the list under `--fleet-open` was
    /// whatever nobody was consulting -- while the other order would leave the node
    /// OPEN for that instant, which is the one direction this whole path exists to
    /// avoid.
    ///
    /// It writes `--fleet-member`'s list and only that, exactly as `Publish` writes the
    /// cluster's and only that. Two publishers, one per question, is what keeps a
    /// reload from discarding what consensus agreed -- which is #251 arriving through
    /// the other door.
    ///
    /// Safe to call while surfaces classify callers on their own threads.
    /// @param cfg The configuration just adopted.
    void Adopt(NodeConfig const& cfg)
    {
        _flagOpen.store(cfg.fleetOpen, std::memory_order_relaxed);
        SettleOpenness();
        _listed.Publish(cfg.fleetMembers);
    }

    /// @copydoc Distributed::IMembershipOracle::Classify
    ///
    /// One atomic read of the flag per request, so a caller is judged by one policy or
    /// the other and never by half of each.
    [[nodiscard]] Distributed::Membership Classify(std::string_view peerAddress) const override
    {
        return _isOpen.load(std::memory_order_relaxed) ? _open.Classify(peerAddress) : _admitted.Classify(peerAddress);
    }

    /// Record what the cluster agreed, alongside what the operator listed.
    ///
    /// The seam consensus drives, and it does nothing under `--fleet-open` -- which
    /// is right rather than an oversight: that flag says "admit everybody", and a
    /// replicated member set narrows nothing an operator has already opened.
    ///
    /// It writes the cluster's list and only that, so `--fleet-member` survives every
    /// commit. Which list is written is decided here rather than passed in, so the
    /// observer consensus installs cannot name the wrong one. A node admitted at
    /// runtime is still served without anybody editing a config file on every other
    /// machine, which is what this seam was for; it simply no longer costs the
    /// operator's own answer to get that.
    ///
    /// It also reads the cluster's `fleet-open` row, which is why it takes the STATE
    /// rather than the endpoint list (#1112). That row was accepted, replicated,
    /// snapshotted and carried across restarts while changing no admission decision,
    /// because admission read the flag and nothing read the row -- the failure
    /// `SettingTable`'s own header describes, one step past the typo `FindSetting`
    /// guards. Two publishers, one per question, would put the openness half on a
    /// second call somebody can forget; one seam settles both, in `Adopt`'s order.
    ///
    /// Safe to call from the consensus thread while surfaces classify callers on
    /// theirs.
    /// @param state The cluster state as of the latest commit.
    void PublishCluster(Cluster::ClusterState const& state)
    {
        // The flag BEFORE the list, exactly as `Adopt` settles them, and for its
        // reason: each half is individually atomic, so what the order decides is
        // which stale half a request between them may read.
        _agreedOpen.store(AgreedOpenness(state), std::memory_order_relaxed);
        SettleOpenness();
        Publish(state.Endpoints());
    }

    /// Record the cluster's member set alone.
    ///
    /// The member-set HALF of `PublishCluster`, and deliberately still reachable: it
    /// is what the member-set cases assert against, and splitting it out keeps those
    /// cases about routing rather than about settings.
    ///
    /// **No production caller may use it, and none can**: the only one is consensus,
    /// whose observer is handed a `ClusterState` and therefore has no endpoint list to
    /// pass. That is what stops this being the second publisher somebody forgets the
    /// openness half on -- the seam's TYPE refuses the mistake rather than a comment
    /// asking people not to make it.
    /// @param endpoints The cluster's members, as `host:port`.
    void Publish(std::vector<std::string> const& endpoints)
    {
        _cluster.Publish(endpoints);
    }

    /// The oracle every surface on this node consults.
    ///
    /// The open one is only ever the operator's stated choice: `--fleet-open` is a
    /// flag rather than what an unset field decays to, so nothing here guesses its
    /// way into serving strangers. Everything else is the union of the two lists,
    /// which for a node that named no members and has agreed nothing admits this
    /// machine and refuses the network -- the safe default rather than a
    /// misconfiguration.
    ///
    /// That default is exactly what a WORKER must be able to leave behind. It could
    /// not until #235: the startup table refused `--fleet-member` on any node
    /// without a scheduler, so a pure worker's oracle was an empty list by
    /// construction and its compile port refused every dispatched job.
    ///
    /// Answers `*this` since #405. It stays a named accessor rather than surfaces
    /// binding the object directly, because the name is what says *this is the seam*
    /// at the call site -- and because a surface that took a `NodeMembership&` could
    /// reach `Adopt`, which belongs to the reload and to nothing else.
    /// @return The oracle; valid for this object's lifetime.
    [[nodiscard]] Distributed::IMembershipOracle const& Oracle() const noexcept
    {
        return *this;
    }

  private:
    /// What the cluster's `fleet-open` row says.
    ///
    /// A named enumeration rather than `-1`/`0`/`1`, because the encoding is the part
    /// that is easy to get wrong: the comment below explains WHY absence is not
    /// closed, and this is what stops a reader having to remember it. `std::int8_t`
    /// so the atomic below is lock-free everywhere this builds.
    /// Each enumerator names what was OBSERVED, never what it was concluded to mean.
    /// `NoRow` and `Unreadable` resolve to the same ANSWER -- this node's own flag --
    /// and are still two enumerators, because they are two different facts about the
    /// cluster: nobody has set it, against somebody has set it to something this build
    /// cannot read. Folding them would make the second unreportable, and the second is
    /// the one that says a NEWER build is writing this row mid rolling upgrade.
    enum class AgreedOpen : std::int8_t
    {
        NoRow,      ///< The state carries no `fleet-open` row at all.
        Unreadable, ///< A row is present and its value is neither `'1'` nor `'0'`.
        Closed,     ///< The row says `'0'`: members only.
        Open,       ///< The row says `'1'`: admit every caller.
    };

    /// What the cluster's `fleet-open` row says, as a tri-state.
    ///
    /// `Absent` is not `Closed`. *Nobody has said* and *somebody said no* are
    /// different facts, and reading the first as the second would close a node whose
    /// operator opened it locally, using a default nobody chose -- the admission-layer
    /// spelling of *absence from `ClusterState` is not removal*. A tri-state rather
    /// than `std::optional<bool>` because this is written on the consensus thread and
    /// read on every surface's, and an optional is not lock-free.
    /// @param state The state to read.
    /// @return What the row says, or `Absent` when there is none this build can read.
    [[nodiscard]] static AgreedOpen AgreedOpenness(Cluster::ClusterState const& state)
    {
        auto const configured = state.SettingOf(Cluster::FleetOpenSetting);
        if (!configured.has_value())
            return AgreedOpen::NoRow;

        // The row's own documented grammar: `'1' to admit every caller, '0' for
        // members only`. Anything else is a value this build cannot act on, and it
        // resolves to ABSENCE rather than to either answer -- guessing `open` would
        // widen admission on a typo, and guessing `closed` would silently override a
        // local flag. `Validate` refuses such a value on the leader before the append,
        // so reaching this needs a NEWER build with a wider grammar, mid rolling
        // upgrade; falling back to the flag is what degrades rather than breaks.
        if (*configured == "1")
            return AgreedOpen::Open;
        if (*configured == "0")
            return AgreedOpen::Closed;
        return AgreedOpen::Unreadable;
    }

    /// Recompute the effective answer from the flag and the cluster's row.
    void SettleOpenness()
    {
        auto const agreed = _agreedOpen.load(std::memory_order_relaxed);
        auto const flag = _flagOpen.load(std::memory_order_relaxed);

        // Absence resolves to the FLAG, never to closed. See `AgreedOpenness`.
        if (agreed == AgreedOpen::NoRow)
        {
            _isOpen.store(flag, std::memory_order_relaxed);
            return;
        }

        // So does a value this build cannot read -- and unlike absence it is worth
        // saying once, because it means a NEWER build is writing this row while this
        // one is still running. Same shape as `AgreedLeaseLifetime`'s fallback (#522):
        // serving under the local answer degrades, refusing every caller would take
        // the node down for an upgrade rather than for a fault.
        if (agreed == AgreedOpen::Unreadable)
        {
            if (!_warnedUnreadable.exchange(true, std::memory_order_relaxed))
                _logger.Logf(LogLevel::Warn,
                             "this cluster's {} is set to something this build cannot read, so this node is using "
                             "its own --fleet-open setting until it is set to '1' or '0'",
                             Cluster::FleetOpenSetting);
            _isOpen.store(flag, std::memory_order_relaxed);
            return;
        }

        auto const wanted = agreed == AgreedOpen::Open;

        // **A keyless node refuses to WIDEN, and narrows freely** -- #282 arriving
        // through a door the reload guard cannot watch. `ValidateNodeReloadable`
        // already refuses this transition when an OPERATOR acts on this machine,
        // because a node with no `--cluster-key-file` built an unchecked lease
        // validator at startup and `MakeWorkerLeaseValidator` has already run. A
        // replicated row does the same thing with **no action on this node at all**,
        // so that guard's own reasoning applies with more force while none of its code
        // runs.
        //
        // Asked as a TRANSITION, like the reload rule: a keyless node its operator has
        // already opened is running happily today and may stay open. Only the widening
        // is refused, and the node then stays CLOSED while the cluster says open --
        // a divergence that fails closed and says so, which is the direction this
        // whole path exists to choose.
        //
        // **It cannot move to the leader, and that is not tidiness left undone.** The
        // leader would have to refuse `--cluster-set fleet-open=1` for the whole
        // cluster on account of one member, and it cannot know whether any other
        // member is keyless -- nothing replicates that, and nothing should.
        if (wanted && !flag && _keyless)
        {
            if (!_warnedKeyless.exchange(true, std::memory_order_relaxed))
                _logger.Logf(LogLevel::Warn,
                             "this cluster has agreed {}=1, and this node has no --cluster-key-file, so it is "
                             "STAYING CLOSED: it built a lease check at startup that verifies nothing, which is "
                             "only safe while no machine but this one is admitted. Give --cluster-key-file and "
                             "restart to join the open fleet.",
                             Cluster::FleetOpenSetting);
            _isOpen.store(false, std::memory_order_relaxed);
            return;
        }

        _isOpen.store(wanted, std::memory_order_relaxed);
    }

    Distributed::OpenMembership _open;

    /// What `--fleet-member` named. Replaced wholesale by `Adopt` on an accepted
    /// reload, and the only route by which a machine that is not a cluster peer is
    /// admitted at all.
    Distributed::ClusterMembership _listed;

    /// What the cluster agreed, replaced on every committed membership change.
    Distributed::ClusterMembership _cluster;

    /// The union the surfaces actually consult. Declared after both participants,
    /// which it borrows.
    Distributed::AnyOfMembership _admitted;

    /// Where the keyless-widening refusal is reported, once.
    ILogger& _logger;

    /// Whether this node started with no `--cluster-key-file`. Fixed for the process;
    /// see the constructor.
    bool _keyless;

    /// Whether this node has already said so once, so a cluster that stays open does
    /// not fill the log with one line per commit.
    std::atomic<bool> _warnedKeyless { false };

    /// What `--fleet-open` says on THIS node. Separate from `_isOpen` since #1112,
    /// which is the whole shape of that ticket: the effective answer is now a
    /// function of two inputs, and keeping the local one is what lets absence resolve
    /// to it and what makes the widening guard a TRANSITION rather than a state.
    std::atomic<bool> _flagOpen;

    /// Whether this node has already reported an unreadable row, so a cluster that
    /// keeps one does not fill the log with a line per commit.
    std::atomic<bool> _warnedUnreadable { false };

    /// What the cluster's `fleet-open` row says; see `AgreedOpenness`.
    std::atomic<AgreedOpen> _agreedOpen { AgreedOpen::NoRow };

    /// Whether `--fleet-open` is in force.
    ///
    /// Atomic since #405, because it is no longer fixed at construction: a reload may
    /// turn it on or off, and the surfaces read it on their own threads. Relaxed
    /// ordering is enough -- it guards nothing but itself, and the list beside it
    /// carries its own lock -- and the branch is one predictable test on a path that
    /// already crosses a network.
    ///
    /// DERIVED since #1112, by `SettleOpenness`, from `_flagOpen` and `_agreedOpen`.
    /// Nothing else writes it, so the two inputs cannot reach the surfaces by
    /// different routes.
    std::atomic<bool> _isOpen;
};

} // namespace FastCache::Node
