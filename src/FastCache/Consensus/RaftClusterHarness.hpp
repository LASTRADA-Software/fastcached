// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/IRandomSource.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace FastCache::Consensus
{

/// A whole Raft cluster running deterministically in one process.
///
/// Test infrastructure, header-only and never linked into the library — the same
/// shape as `Cache/StorageTestUtils.hpp` next door.
///
/// ## Why this exists rather than more unit tests
///
/// Every rule in this module has a case pinning it in isolation, and that is not
/// the same as the algorithm being right. Raft's guarantees are about what can
/// never happen across *all* interleavings of message loss, reordering,
/// partitions and restarts, and no amount of single-transition testing reaches
/// them. A hand-written consensus implementation also has no published
/// verification vector to check itself against, the way `MurmurHash3` has
/// SMHasher's — so the paper's safety properties, asserted continuously while an
/// adversary shakes the cluster, are the closest thing to an oracle available.
///
/// Determinism is the point: time only moves when `Step` says so, and every
/// network decision comes from an injected `IRandomSource`, so a failure is
/// reproducible from its seed rather than being a flake to re-run.
class RaftClusterHarness
{
  public:
    /// One node's whole world: its storage, its transport, and what it applied.
    struct Member;

    /// @param members Every node in the cluster.
    /// @param seedOffset Staggers each node's election timer draws, so a cluster
    ///        whose members all draw identically does not split its vote forever.
    explicit RaftClusterHarness(std::vector<NodeId> members, std::uint64_t seedOffset = 1);

    /// Advance the clock, deliver what is due, and tick every node.
    ///
    /// Invariants are checked after every step rather than at the end, so a
    /// violation is reported at the moment it appears instead of being inferred
    /// from wreckage later.
    /// @param by How far to advance.
    void Step(std::chrono::milliseconds by = std::chrono::milliseconds { 10 });

    /// Run `Step` repeatedly.
    /// @param steps How many.
    /// @param by How far each advances.
    void Run(std::size_t steps, std::chrono::milliseconds by = std::chrono::milliseconds { 10 });

    /// Cut the cluster in two; messages crossing the line are dropped.
    /// @param side The nodes on one side. Everything else is on the other.
    void Partition(std::set<NodeId> side);

    /// Remove any partition.
    void Heal();

    /// Drop this fraction of messages, in parts per hundred.
    /// @param percent 0 delivers everything, 100 delivers nothing.
    void SetLossPercent(std::uint64_t percent) noexcept;

    /// Restart a node: a fresh `RaftNode` recovered from its own storage.
    ///
    /// Which is exactly what a process restart looks like from the algorithm's
    /// side, and the reason the storage lives in the harness rather than in the
    /// node.
    /// @param who Which node.
    void Restart(NodeId const& who);

    /// Every node currently claiming to be leader.
    /// @return Their ids.
    [[nodiscard]] std::vector<NodeId> Leaders() const;

    /// The leader holding the highest term, when any node leads.
    /// @return Its id, or nullopt.
    [[nodiscard]] std::optional<NodeId> Leader() const;

    /// The term of the node `Leader()` names.
    /// @return Its term, or nullopt when nobody leads.
    [[nodiscard]] std::optional<Term> TermOfLeader() const;

    /// Offer an entry through whichever node leads.
    /// @param payload The bytes.
    /// @return Where it landed, or nullopt when nobody leads.
    [[nodiscard]] std::optional<LogIndex> ProposeOnLeader(std::vector<std::byte> payload);

    /// @param who Which node.
    /// @return That node's view.
    [[nodiscard]] Member const& At(NodeId const& who) const;

    /// The current instant.
    [[nodiscard]] TimePoint Now() const noexcept;

    /// Whether every safety property has held at every step so far.
    /// @return Empty when all is well, else what broke and when.
    [[nodiscard]] std::vector<std::string> const& Violations() const noexcept;

    /// One node's world.
    struct Member
    {
        NodeId id;

        /// Declared BEFORE `driver`, and the order is load-bearing: members are
        /// destroyed in reverse, and the `RaftNode` the driver owns holds an
        /// `IRandomSource&` into this. The other way round it would outlive the
        /// source it references — harmless only for as long as no destructor
        /// happens to draw.
        std::unique_ptr<SystemRandomSource> random;

        std::unique_ptr<InMemoryRaftStorage> storage;
        std::unique_ptr<IRaftTransport> transport;
        std::unique_ptr<IRaftStateMachine> machine;
        std::unique_ptr<RaftDriver> driver;

        /// What this node has applied, in order. The state machine's whole job.
        std::vector<AppliedEntry> applied;
    };

  private:
    struct InFlight
    {
        NodeId from;
        NodeId to;
        RaftMessage message;
        TimePoint deliverAt;
    };

    /// Collects sends into the harness's own queue.
    class QueueingTransport final: public IRaftTransport
    {
      public:
        QueueingTransport(RaftClusterHarness& harness, NodeId from) noexcept:
            _harness { harness },
            _from { std::move(from) }
        {
        }

        void Send(NodeId const& to, RaftMessage message) override
        {
            _harness.Enqueue(_from, to, std::move(message));
        }

      private:
        RaftClusterHarness& _harness;
        NodeId _from;
    };

    /// Records what a node applied, which is what State Machine Safety is about.
    class RecordingMachine final: public IRaftStateMachine
    {
      public:
        RecordingMachine(RaftClusterHarness& harness, NodeId who) noexcept:
            _harness { harness },
            _who { std::move(who) }
        {
        }

        void Apply(AppliedEntry const& entry) override
        {
            _harness.RecordApplied(_who, entry);
        }

        [[nodiscard]] std::vector<std::byte> TakeSnapshot() override
        {
            return {};
        }

        void RestoreSnapshot(std::span<std::byte const> state) override
        {
            std::ignore = state;
            // Nothing to restore: this machine records what it was told rather
            // than holding state, and the safety properties the harness checks
            // are about the LOG, which the node has already replaced.
        }

      private:
        RaftClusterHarness& _harness;
        NodeId _who;
    };

    /// The configuration every node in this cluster runs with.
    ///
    /// One definition rather than two identical literals: a restart has to
    /// reconstruct the same configuration the node started with, and a second copy
    /// is one that can drift -- a restarted node quietly running different
    /// timeouts would change what the simulation is testing without saying so.
    /// @param who Which member the configuration is for.
    /// @return The configuration.
    [[nodiscard]] RaftConfig ConfigFor(NodeId const& who) const;

    void Enqueue(NodeId const& from, NodeId const& to, RaftMessage message);
    void RecordApplied(NodeId const& who, AppliedEntry const& entry);
    [[nodiscard]] bool Reaches(NodeId const& from, NodeId const& to) const;
    void CheckInvariants();

    [[nodiscard]] Member& Find(NodeId const& who);

    ManualClock _clock;

    /// Drives delay and loss decisions. Seeded fixed, so a failure is replayable
    /// from the same seed rather than being a flake to re-run.
    SystemRandomSource _network { 0x5EED };
    std::vector<NodeId> _members;
    std::vector<std::unique_ptr<Member>> _nodes;
    std::vector<InFlight> _wire;

    std::optional<std::set<NodeId>> _partition;
    std::uint64_t _lossPercent { 0 };

    /// Which node was seen leading each term, for Election Safety.
    std::map<std::uint64_t, NodeId> _leaderOfTerm;

    /// What was committed at each index, and in which term.
    ///
    /// The term is not decoration. Leader Completeness constrains the leaders of
    /// terms *higher* than the one an entry was committed in, and says nothing
    /// about a leader still sitting on an older term -- which is a state that
    /// really occurs, because a leader cut off from its peers is never told it has
    /// been deposed. Checking every leader against every committed index without
    /// that comparison reports the algorithm broken when it is behaving exactly as
    /// specified.
    struct Committed
    {
        std::vector<std::byte> payload;
        std::uint64_t term {};
    };

    std::map<std::uint64_t, Committed> _appliedAt;

    std::vector<std::string> _violations;
};

// ---------------------------------------------------------------------------
// Implementation. Inline because this is header-only test infrastructure: the
// glob that builds the test binary picks up `*_test.cpp` only, so a separate
// translation unit here would never be compiled.

inline RaftClusterHarness::RaftClusterHarness(std::vector<NodeId> members, std::uint64_t seedOffset):
    _members { std::move(members) }
{
    for (auto index = std::size_t { 0 }; index < _members.size(); ++index)
    {
        auto member = std::make_unique<Member>();
        member->id = _members[index];
        member->storage = std::make_unique<InMemoryRaftStorage>();
        member->transport = std::make_unique<QueueingTransport>(*this, member->id);
        member->machine = std::make_unique<RecordingMachine>(*this, member->id);

        // A distinct seed per node, because the whole point of a randomized
        // election timeout is that two nodes do not draw the same one -- and a
        // cluster whose members all drew identically would split its vote, retry,
        // and split it again for as long as the test ran.
        member->random = std::make_unique<SystemRandomSource>((seedOffset * 1000) + index);

        auto node = RaftNode::Create(ConfigFor(member->id), *member->random, _clock.Now());
        member->driver =
            std::make_unique<RaftDriver>(std::move(node).value(), *member->storage, *member->transport, *member->machine);

        _nodes.push_back(std::move(member));
    }
}

inline RaftConfig RaftClusterHarness::ConfigFor(NodeId const& who) const
{
    return RaftConfig { .self = who,
                        .members = _members,
                        .electionTimeoutMin = std::chrono::milliseconds { 150 },
                        .electionTimeoutMax = std::chrono::milliseconds { 300 },
                        .heartbeatInterval = std::chrono::milliseconds { 50 } };
}

inline RaftClusterHarness::Member& RaftClusterHarness::Find(NodeId const& who)
{
    for (auto& node: _nodes)
        if (node->id == who)
            return *node;

    // Loudly, rather than falling back to the first node. A silent fallback makes
    // `Restart("n4")` on a three-node cluster restart n1 and the case pass while
    // asserting nothing about what it named -- a vacuous test indistinguishable
    // from a real one, which is the failure shape .agent/rules/compile-cache.md
    // already records for the Windows cross-depth e2e case.
    throw std::out_of_range { "no such cluster member: " + who };
}

inline RaftClusterHarness::Member const& RaftClusterHarness::At(NodeId const& who) const
{
    for (auto const& node: _nodes)
        if (node->id == who)
            return *node;

    throw std::out_of_range { "no such cluster member: " + who };
}

inline TimePoint RaftClusterHarness::Now() const noexcept
{
    return _clock.Now();
}

inline std::vector<std::string> const& RaftClusterHarness::Violations() const noexcept
{
    return _violations;
}

inline void RaftClusterHarness::SetLossPercent(std::uint64_t percent) noexcept
{
    _lossPercent = percent;
}

inline void RaftClusterHarness::Partition(std::set<NodeId> side)
{
    _partition = std::move(side);
}

inline void RaftClusterHarness::Heal()
{
    _partition.reset();
}

inline bool RaftClusterHarness::Reaches(NodeId const& from, NodeId const& to) const
{
    if (!_partition.has_value())
        return true;

    return _partition->contains(from) == _partition->contains(to);
}

inline void RaftClusterHarness::Enqueue(NodeId const& from, NodeId const& to, RaftMessage message)
{
    if (_lossPercent > 0 && _network.UniformInRange(1, 100) <= _lossPercent)
        return;

    // A delay drawn per message, which is what produces reordering: two messages
    // sent in one step can arrive in either order, and Raft has to be indifferent
    // to that rather than merely usually survive it.
    auto const delay = _network.UniformInRange(1, 3);
    _wire.push_back(InFlight { .from = from,
                               .to = to,
                               .message = std::move(message),
                               .deliverAt = _clock.Now() + std::chrono::milliseconds { delay * 5 } });
}

inline void RaftClusterHarness::RecordApplied(NodeId const& who, AppliedEntry const& entry)
{
    Find(who).applied.push_back(entry);

    // State Machine Safety, checked where it happens: no two nodes may apply
    // different commands at the same index. This is the property everything else
    // in the algorithm exists to produce, so it is asserted against the actual
    // application rather than inferred from logs.
    auto const term = Find(who).driver->Node().CurrentTerm().value;

    auto const seen = _appliedAt.find(entry.index.value);
    if (seen == _appliedAt.end())
    {
        _appliedAt.emplace(entry.index.value, Committed { .payload = entry.payload, .term = term });
        return;
    }

    if (seen->second.payload != entry.payload)
        _violations.push_back("State Machine Safety: two nodes applied different commands at index "
                              + std::to_string(entry.index.value));

    // The earliest term it was seen committed in is the one the property is
    // stated against, so keep the lowest rather than the latest.
    seen->second.term = std::min(seen->second.term, term);
}

inline void RaftClusterHarness::CheckInvariants()
{
    for (auto const& node: _nodes)
    {
        auto const& raft = node->driver->Node();

        if (raft.CurrentRole() == Role::Leader)
        {
            // Election Safety: at most one leader per term.
            auto const term = raft.CurrentTerm().value;
            auto const claimed = _leaderOfTerm.find(term);
            if (claimed == _leaderOfTerm.end())
                _leaderOfTerm.emplace(term, node->id);
            else if (claimed->second != node->id)
                _violations.push_back("Election Safety: both " + claimed->second + " and " + node->id + " led term "
                                      + std::to_string(term));

            // Leader Completeness: an entry committed in term T is present, with
            // the same bytes, in the log of every leader of a term above T.
            // Leaders at or below T are deliberately not checked -- see the note
            // on `Committed`.
            for (auto const& [index, committed]: _appliedAt)
            {
                if (term <= committed.term)
                    continue;

                // Stated as "must hold exactly this" rather than as a list of ways
                // it may be wrong. Enumerating the failures left a hole: only
                // `_appliedAt` records commands, so a leader holding a *no-op*
                // where a command was committed is a divergence of exactly the
                // kind this property forbids, and a check that only compared
                // payloads when the kinds already matched waved it through.
                auto const* const held = raft.Log().EntryAt(LogIndex { .value = index });
                auto const correct =
                    held != nullptr && held->kind == EntryKind::Command && held->payload == committed.payload;

                if (!correct)
                    _violations.push_back("Leader Completeness: leader " + node->id + " of term " + std::to_string(term)
                                          + " does not hold the entry committed at index " + std::to_string(index)
                                          + " in term " + std::to_string(committed.term));
            }
        }
    }

    // Log Matching: two logs sharing an (index, term) must agree on every entry
    // up through that index.
    for (auto outer = std::size_t { 0 }; outer < _nodes.size(); ++outer)
    {
        for (auto inner = outer + 1; inner < _nodes.size(); ++inner)
        {
            auto const& left = _nodes[outer]->driver->Node().Log();
            auto const& right = _nodes[inner]->driver->Node().Log();
            auto const shared = std::min(left.LastIndex().value, right.LastIndex().value);

            for (auto index = shared; index >= 1; --index)
            {
                auto const at = LogIndex { .value = index };
                if (left.TermAt(at) != right.TermAt(at))
                    continue;

                // Agreed here, so every entry below must be identical.
                for (auto below = index; below >= 1; --below)
                {
                    auto const* const a = left.EntryAt(LogIndex { .value = below });
                    auto const* const b = right.EntryAt(LogIndex { .value = below });
                    if (a != nullptr && b != nullptr && (a->term != b->term || a->payload != b->payload))
                    {
                        _violations.push_back("Log Matching: " + _nodes[outer]->id + " and " + _nodes[inner]->id
                                              + " agree at index " + std::to_string(index) + " but differ at "
                                              + std::to_string(below));
                        break;
                    }
                }

                break;
            }
        }
    }
}

inline void RaftClusterHarness::Step(std::chrono::milliseconds by)
{
    _clock.Advance(by);
    auto const now = _clock.Now();

    auto due = std::vector<InFlight> {};
    auto waiting = std::vector<InFlight> {};
    for (auto& message: _wire)
    {
        if (message.deliverAt <= now)
            due.push_back(std::move(message));
        else
            waiting.push_back(std::move(message));
    }

    _wire = std::move(waiting);

    for (auto const& message: due)
    {
        // A partition drops in flight as well as on send: a message already on
        // the wire when the network splits does not arrive either.
        if (!Reaches(message.from, message.to))
            continue;

        (void) Find(message.to).driver->Receive(message.message, now);
    }

    for (auto& node: _nodes)
        (void) node->driver->Tick(now);

    CheckInvariants();
}

inline void RaftClusterHarness::Run(std::size_t steps, std::chrono::milliseconds by)
{
    for (auto step = std::size_t { 0 }; step < steps; ++step)
        Step(by);
}

inline std::vector<NodeId> RaftClusterHarness::Leaders() const
{
    auto found = std::vector<NodeId> {};
    for (auto const& node: _nodes)
        if (node->driver->Node().CurrentRole() == Role::Leader)
            found.push_back(node->id);

    return found;
}

inline std::optional<NodeId> RaftClusterHarness::Leader() const
{
    // The highest-term leader, not "the one leader", because more than one node
    // can truthfully report itself leader at the same moment. A leader cut off
    // from its peers does not find out it has been deposed -- plain Raft has no
    // rule that makes it step down, and adding one (CheckQuorum, or a leader
    // lease) is a separate mechanism this library does not yet have. What still
    // holds is the guarantee that matters: the stale one cannot commit anything,
    // because committing needs a quorum it cannot reach.
    //
    // This is not a loophole in Election Safety, which is per *term*: the stale
    // leader holds an older term than the one the majority elected.
    auto best = std::optional<NodeId> {};
    auto bestTerm = Term::None();

    for (auto const& node: _nodes)
    {
        auto const& raft = node->driver->Node();
        if (raft.CurrentRole() != Role::Leader)
            continue;

        if (!best.has_value() || raft.CurrentTerm() > bestTerm)
        {
            best = node->id;
            bestTerm = raft.CurrentTerm();
        }
    }

    return best;
}

inline std::optional<Term> RaftClusterHarness::TermOfLeader() const
{
    auto const leader = Leader();
    if (!leader.has_value())
        return std::nullopt;

    return At(*leader).driver->Node().CurrentTerm();
}

inline std::optional<LogIndex> RaftClusterHarness::ProposeOnLeader(std::vector<std::byte> payload)
{
    auto const leader = Leader();
    if (!leader.has_value())
        return std::nullopt;

    auto const proposed = Find(*leader).driver->Propose(std::move(payload), _clock.Now());
    if (!proposed.has_value())
        return std::nullopt;

    return *proposed;
}

inline void RaftClusterHarness::Restart(NodeId const& who)
{
    auto& member = Find(who);

    auto recovered = member.storage->Load();
    if (!recovered.has_value())
        return;

    auto node = RaftNode::Create(ConfigFor(member.id), *member.random, _clock.Now(), std::move(recovered).value());
    member.driver =
        std::make_unique<RaftDriver>(std::move(node).value(), *member.storage, *member.transport, *member.machine);
}

} // namespace FastCache::Consensus
