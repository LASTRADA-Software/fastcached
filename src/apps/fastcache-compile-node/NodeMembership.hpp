// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "NodeConfig.hpp"

#include <FastCache/Cluster/ClusterState.hpp>
#include <FastCache/Cluster/NodeMode.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/MembershipOracle.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Node
{

/// Publish which identity keys @p state holds live and which it revoked, into @p keys.
///
/// Members of either seat and enrolled principals are live under their ids; a revoked key is
/// revoked whatever id it was revoked under. In one swap, so a reader never sees a key admitted by
/// one roster and revoked by the next as neither. The ONE derivation from state to key admission:
/// `NodeMembership::PublishCluster` calls it, and so does a test fleet whose machines must admit
/// exactly what production would, never a copy of this loop.
/// @param keys The roster the surfaces consult.
/// @param state The cluster state as of the latest commit.
inline void PublishClusterKeys(Distributed::KeyRosterMembership& keys, Cluster::ClusterState const& state)
{
    std::map<std::string, Ed25519PublicKey, std::less<>> live;
    for (auto const& member: state.members)
        live.emplace(member.id, member.publicKey);
    for (auto const& principal: state.principals)
        live.emplace(principal.id, principal.publicKey);
    std::vector<Ed25519PublicKey> revoked;
    revoked.reserve(state.revokedKeys.size());
    for (auto const& entry: state.revokedKeys)
        revoked.push_back(entry.publicKey);
    keys.Publish(std::move(live), std::move(revoked));
}

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
/// ## A credential, not an address
///
/// A machine that is not this one is admitted by what it can PROVE -- a live identity key,
/// through a session proof or a ticket its own key signed -- or by `--fleet-open`, never by
/// the address it dials from. A client machine, which is no cluster peer and never will be,
/// is admitted the same way as a member: the roster names its key.

/// Whether this configuration admits a machine that is not this one.
///
/// The routes `NodeMembership`'s constructor composes, asked of the configuration instead of
/// a caller, and it lives HERE for that reason: a reader adding a route has this function in
/// front of them. Left elsewhere it was a second reader of the same policy with nothing
/// pointing at it, and a new route would silently stop the startup rule firing: an open
/// compile port, every refusal counter at zero, and a fleet green from both ends.
///
/// **The route the rulebook promised has arrived, and it is a CREDENTIAL**: a roster admits
/// machines by proof and by ticket, whatever address they dial from. So the question needs to
/// know what the caller counts of that route, and the caller states it: `Absent` counts none,
/// `Unknown` counts it on every node running consensus -- the only node that holds a roster, its
/// applied state -- which is the fail-closed reading a guard wants, and `Formed` is consensus's own
/// roster, which admits exactly the members the formation half below names. Removing a route fails OPEN through exactly this function,
/// because a worker that reads it as false builds a lease check that verifies nothing.
///
/// It cannot be answered by asking the oracle, which is why it is a separate function rather
/// than a method. `Oracle()` answers "is this caller admitted" in the present tense; this asks
/// whether the admitted set will EVER contain another machine, and the key roster is empty at
/// construction by design. The formation half predicts the roster consensus will later publish
/// into this object -- peers admitted by the keys they prove, never by an address -- which no
/// runtime query can see.
/// @param cfg The parsed configuration.
/// @param roster What the caller knows about the roster this node verifies keys against.
/// @return Whether the policy admits anything but this machine.
[[nodiscard]] inline bool AdmitsRemotePeers(NodeConfig const& cfg, RosterPresence roster)
{
    // `--fleet-open` first: it admits every machine there is, and says so as a flag
    // rather than as an absence, so nothing here has to infer it.
    if (cfg.fleetOpen)
        return true;

    // The fleet a formed node is in, or is asking into: its mode row's `members` column, which
    // says for a pending node too that other machines are about to be members -- the roster
    // consensus will later publish, peers admitted by the keys they prove, which no runtime query
    // can see yet. Unless its consensus is confined to this machine (`ConsensusConfinedToThisMachine`):
    // every way of becoming more than one machine is then shut, a pending node's ask included, so
    // that roster can never name another machine.
    if (cfg.formation.has_value() && Cluster::NodeModeRowFor(cfg.formation->mode).members == Cluster::FleetReach::Beyond
        && !ConsensusConfinedToThisMachine(cfg))
        return true;

    // The key routes: a proof or a VERIFIED ticket admits only against a roster -- never an AUTH
    // that merely answered `Ok`, which is why the question is the roster's presence.
    return roster == RosterPresence::Unknown && RunsConsensus(cfg);
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
    /// @param logger Where an unreadable `fleet-open` row is reported, once.
    NodeMembership(NodeConfig const& cfg, ILogger& logger):
        _loopback {},
        _open {},
        // Pointers into this object's own members, which is safe because the type is
        // neither copyable nor movable and the composites are declared after all of them.
        _keys {},
        // This machine, and whatever key a connection proved or presented. No address admits a
        // machine that is not this one: a replicated member set is keys now, never hosts.
        _admitted { { &_loopback, &_keys } },
        // `--fleet-open` is folded with the key roster rather than replacing it, and that
        // is a decision about what the flag MEANS (#1309, #178). It says "I have not
        // enumerated who may use this fleet; serve whoever asks" -- a blanket over
        // machines nobody named. A forget names one, by revoking its key, and a revoked key
        // is `Forgotten`. Letting the blanket win would make the local flag resurrect a
        // machine the cluster positively removed, on exactly the node nobody has
        // reconfigured yet.
        //
        // The directions are not comparable, which is what settles it: honouring the
        // forget wrongly refuses a machine, fails CLOSED, and is visible from the
        // refused end. Ignoring it serves a decommissioned machine indefinitely, fails
        // OPEN, and admission succeeding is the ordinary case -- nothing reports it.
        _openly { { &_loopback, &_open, &_keys } },
        _logger { logger },
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
    /// **The removal direction is what this exists for.** `--fleet-open` turned ON and
    /// not yet in force fails closed -- a caller is refused until somebody restarts the
    /// node, which is annoying, self-healing and visible. Turned OFF and still in force
    /// it fails OPEN: every caller the roster does not admit keeps being served, and
    /// nothing reports it, because admission succeeding is the ordinary case.
    ///
    /// It settles openness and only that, exactly as `PublishCluster` writes the
    /// cluster's facts and only those: a reload that rebuilt the cluster's half from a
    /// config file would discard what consensus agreed, which is #251 arriving through
    /// the other door.
    ///
    /// Safe to call while surfaces classify callers on their own threads.
    /// @param cfg The configuration just adopted.
    void Adopt(NodeConfig const& cfg)
    {
        _flagOpen.store(cfg.fleetOpen, std::memory_order_relaxed);
        SettleOpenness();
    }

    /// @copydoc Distributed::IMembershipOracle::Explain
    ///
    /// One atomic read of the flag per request, so a caller is judged by one policy or
    /// the other and never by half of each.
    ///
    /// It attributes nothing of its own: whichever policy answered names ITSELF, so an
    /// operator reading a refusal sees `Loopback`, `OpenPolicy` or a key route
    /// rather than "the node" -- which is the question #1471 asks
    /// and the one this delegation is already the right shape for.
    [[nodiscard]] Distributed::MembershipDecision Explain(std::string_view peerAddress) const override
    {
        return _isOpen.load(std::memory_order_relaxed) ? _openly.Explain(peerAddress) : _admitted.Explain(peerAddress);
    }

    /// @copydoc Distributed::IMembershipOracle::ExplainKey
    ///
    /// Through the same fold `Explain` answers from, whichever it is, so opening a node changes what
    /// an ADDRESS is worth and never what a revoked key is worth.
    [[nodiscard]] Distributed::MembershipDecision ExplainKey(ProvenIdentity const& identity,
                                                             Distributed::KeyEvidence evidence) const override
    {
        return _isOpen.load(std::memory_order_relaxed) ? _openly.ExplainKey(identity, evidence)
                                                       : _admitted.ExplainKey(identity, evidence);
    }

    /// @copydoc Distributed::IMembershipOracle::LiveKeyOf
    [[nodiscard]] std::optional<Ed25519PublicKey> LiveKeyOf(std::string_view id) const override
    {
        return _isOpen.load(std::memory_order_relaxed) ? _openly.LiveKeyOf(id) : _admitted.LiveKeyOf(id);
    }

    /// How many `--cluster-forget-client` tombstones this node has APPLIED (#1471).
    ///
    /// The seam consensus drives. It writes the cluster's facts and only those, so a reload's
    /// `--fleet-open` survives every commit. A machine admitted at runtime is served without
    /// anybody editing a config file on every other machine, which is what this seam was for --
    /// admitted by its KEY, which is the one thing it publishes about machines: no address
    /// joins admission, so a member's endpoint is not published here at all.
    ///
    /// It also reads the cluster's `fleet-open` row, which is why it takes the STATE (#1112).
    /// That row was accepted, replicated, snapshotted and carried across restarts while changing
    /// no admission decision, because admission read the flag and nothing read the row. Two
    /// publishers, one per question, would put the openness half on a second call somebody can
    /// forget; one seam settles both, in `Adopt`'s order.
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

        // Which identity keys are live and which are revoked (#178).
        PublishClusterKeys(_keys, state);
    }

    /// The oracle every surface on this node consults.
    ///
    /// The open one is only ever the operator's stated choice: `--fleet-open` is a
    /// flag rather than what an unset field decays to, so nothing here guesses its
    /// way into serving strangers. Everything else is this machine plus the fold of the
    /// cluster's keys, which for a node that holds no roster admits this machine and
    /// refuses the network -- the safe default rather than a misconfiguration.
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

        // No unchecked-node exception, and its absence is deliberate (#1308). One stood
        // here: a node with nothing to check a grant against built a lease check that
        // verifies nothing, so a replicated `fleet-open=1` widening it was #282 through a
        // door the reload guard cannot watch. Cluster state reaches only a node running
        // consensus, and consensus verifies every grant against the state it applies (#178),
        // so every node this row can open checks the grants its new callers present. The
        // RELOAD guard stays: a node that runs no consensus can still check nothing and be
        // opened by its operator.
        _isOpen.store(agreed == AgreedOpen::Open, std::memory_order_relaxed);
    }

    /// This machine, whatever the roster says (`Loopback`).
    Distributed::LoopbackMembership _loopback;

    Distributed::OpenMembership _open;

    /// Which identity keys the cluster holds live and which it revoked (#178). Written by
    /// `PublishCluster` and by nothing else -- deliberately NOT by `Adopt`: a revocation is the
    /// cluster's fact, and a reload that rebuilt it from a config file would erase every forget
    /// agreed since startup, which is #251's shape in the one direction where the erasure fails
    /// open.
    Distributed::KeyRosterMembership _keys;

    /// The fold the surfaces consult when this node has not been opened. Declared
    /// after the participants it borrows.
    Distributed::AnyOfMembership _admitted;

    /// The fold they consult when it has. `_open` admits everybody and a revoked key in
    /// `_keys` still outranks it -- and `_loopback` too, so this machine is attributed as
    /// itself; see the constructor for why the flag does not simply win.
    Distributed::AnyOfMembership _openly;

    /// Where an unreadable `fleet-open` row is reported, once.
    ILogger& _logger;

    /// What `--fleet-open` says on THIS node. Separate from `_isOpen` since #1112,
    /// which is the whole shape of that ticket: the effective answer is now a
    /// function of two inputs, and keeping the local one is what lets absence resolve
    /// to it.
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
