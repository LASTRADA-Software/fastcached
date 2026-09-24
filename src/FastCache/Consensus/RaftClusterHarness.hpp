// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Consensus/IRaftPeerIdentity.hpp>
#include <FastCache/Consensus/InMemoryRaftStorage.hpp>
#include <FastCache/Consensus/RaftDriver.hpp>
#include <FastCache/Consensus/RaftOutput.hpp>
#include <FastCache/Consensus/RaftPeerSession.hpp>
#include <FastCache/Consensus/RaftWire.hpp>
#include <FastCache/Core/IRandomSource.hpp>
#include <FastCache/Core/ISecureRandom.hpp>
#include <FastCache/Core/SessionSeal.hpp>
#include <FastCache/Core/WireFields.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <core/platform/Clock.hpp>

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
///
/// ## Every message proves who sent it (#1308, #178)
///
/// Each delivery runs the peer wire's whole exchange between the sender's identity and
/// the receiver's -- challenge, proof, verdict, a frame sealed under the key the two agreed
/// and opened at the other end -- through the SAME `RaftPeerSession` objects the production
/// transport and server drive, and the receiver is handed what it decoded rather than what
/// was sent. One session per message rather than per link: the harness has no connections,
/// and a session that outlived a partition or a restart would be a property of the harness,
/// not of the wire.
///
/// So a cluster formed here is one whose every message was signed by the member it names,
/// and a machine that is not that member -- claiming an id whose key it does not hold, or
/// holding a key the roster revoked -- is a case (`Intrude`, `Join` with an identity) rather
/// than a caveat. The identity factory is REQUIRED at construction: a harness that defaulted
/// one would let a case forget who its members are and still read as a cluster that forms.
/// Nonces and ephemeral keys come from a source of their own, so adding the handshake moved
/// none of the delay and loss draws an existing seed produces.
///
/// A machine's NAME on this network and the id it proves are two things, and are the same
/// for every honest member: messages are routed by name, and the node inside runs as the id
/// its identity proves. That is what lets `Intrude` put a machine on the network that CLAIMS
/// an existing member's id, which a network keyed by one of them could not express.
class RaftClusterHarness
{
  public:
    /// One node's whole world: its storage, its transport, and what it applied.
    struct Member;

    /// Makes the identity a member proves itself with, as sender and as receiver.
    using IdentityFactory = std::function<std::unique_ptr<IRaftPeerIdentity const>(NodeId const& who)>;

    /// @param members Every node in the cluster, each one a voter.
    /// @param identities Who each member, and each later `Join`, is.
    /// @param seedOffset Staggers each node's election timer draws, so a cluster
    ///        whose members all draw identically does not split its vote forever.
    /// @param compaction When every node trims its log into a snapshot; never by
    ///        default, as for a driver. A case about recovery or installation from a
    ///        snapshot names a threshold, and every node -- restarted or joined later --
    ///        runs with it.
    RaftClusterHarness(std::vector<NodeId> members,
                       IdentityFactory identities,
                       std::uint64_t seedOffset = 1,
                       CompactionPolicy compaction = {});

    /// A cluster bootstrapped with voters AND learners (#1449).
    ///
    /// Every node named in either set is started, each with the whole configuration
    /// as its bootstrap -- which is how a learner a configuration names on its own
    /// command line comes up.
    /// @param configuration The voters and the learners.
    /// @param identities Who each member, and each later `Join`, is.
    /// @param seedOffset As for the other constructor.
    /// @param compaction As for the other constructor.
    RaftClusterHarness(Configuration configuration,
                       IdentityFactory identities,
                       std::uint64_t seedOffset = 1,
                       CompactionPolicy compaction = {});

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
    /// node. And from the APPLICATION's side (#1542): its state is emptied first,
    /// because a process that restarts remembers nothing it applied, so whatever
    /// the node holds afterwards is what recovery gave back -- the snapshot's
    /// state, restored, and the entries above it, re-applied.
    ///
    /// **A restart can be REFUSED**, through the one seam production's is:
    /// `RaftDriver::Create`, when the application cannot read what the node recovered.
    /// The node is then DOWN -- no driver, reached by no message and ticked by no step
    /// -- which is what a process that refused to start is, and its application holds
    /// nothing, because the refusal handed it nothing.
    /// @param who Which node.
    /// @return Nothing once it runs again; otherwise why it does not, and it is down.
    [[nodiscard]] std::expected<void, ConsensusError> Restart(NodeId const& who);

    /// Make a node's application unable, or able again, to read any snapshot.
    ///
    /// What a member running a build that reads another state format looks like from
    /// the consensus layer's side (#1552): every snapshot a leader offers it is one its
    /// application cannot take on. Lifting it is that member being upgraded to the
    /// format its leader writes -- and a node that stayed behind rather than taking a
    /// snapshot on catches up by itself from there.
    /// @param who Which node.
    /// @param unreadable Whether snapshots are now beyond it.
    void SetSnapshotsUnreadable(NodeId const& who, bool unreadable);

    /// Bring up a machine that has no cluster, on the same network.
    ///
    /// The shape a node being added to a running fleet actually has: an empty
    /// configuration, an empty log, and nothing to do until somebody's leader
    /// speaks to it. It is reachable from every existing node from this moment,
    /// which is the point — being reachable is what a bootstrap list buys the
    /// others, and admission is what has to supply it here.
    ///
    /// Deliberately **not** added to the harness's bootstrap set: that set is what
    /// `Restart` reconstructs a configuration from, and a joiner's configuration
    /// after a restart must come from its own log rather than from a list nobody
    /// gave it.
    /// @param who The new node's id; must not already exist.
    void Join(NodeId const& who);

    /// `Join`, with an identity of the case's choosing rather than the cluster's.
    ///
    /// The joiner an operator admitted by id whose key the roster does not hold: the
    /// leader's configuration names it, and nothing it is sent may reach it.
    /// @param who The new node's name on the network; must not already exist.
    /// @param identity Who it proves itself as, and with which key.
    void Join(NodeId const& who, std::unique_ptr<IRaftPeerIdentity const> identity);

    /// Put a machine on the network that claims membership and cannot prove it.
    ///
    /// Bootstrapped with every member AND the id it claims, so it campaigns: its election
    /// timer expires and it asks every member for a vote, which is the traffic an intruder
    /// that could be heard would use to disrupt a cluster. It is not added to the harness's
    /// member list, and nothing counts it towards a quorum.
    ///
    /// The id it runs as is its IDENTITY's, which may be an existing member's: a machine
    /// holding every byte it ever held, claiming the id of a member whose key it does not
    /// hold. Messages addressed to that id still reach the member, since the network routes
    /// by name.
    /// @param who Its name on the network; must not already exist.
    /// @param identity What it proves itself as.
    void Intrude(NodeId const& who, std::unique_ptr<IRaftPeerIdentity const> identity);

    /// How many messages addressed to a node were refused before reaching it.
    /// @param who The receiver.
    /// @return The count, zero for a node that has refused nothing.
    [[nodiscard]] std::uint64_t RefusedAt(NodeId const& who) const;

    /// How many messages a node sent that reached their receiver.
    /// @param who The sender.
    /// @return The count, zero for a node nothing was delivered from.
    [[nodiscard]] std::uint64_t DeliveredFrom(NodeId const& who) const;

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

    /// Offer a new configuration through whichever node leads.
    /// @param configuration The proposed voters and learners.
    /// @return Where it landed, or nullopt when nobody leads or it was refused.
    [[nodiscard]] std::optional<LogIndex> ProposeMembershipOnLeader(Configuration configuration);

    /// @param who Which node.
    /// @return That node's view.
    [[nodiscard]] Member const& At(NodeId const& who) const;

    /// The current instant.
    [[nodiscard]] core::platform::SteadyTimePoint Now() const noexcept;

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

        /// Who this node proves itself as, as sender and as receiver; the id its Raft node
        /// runs as is this identity's.
        std::unique_ptr<IRaftPeerIdentity const> identity;

        std::unique_ptr<InMemoryRaftStorage> storage;
        std::unique_ptr<IRaftTransport> transport;
        std::unique_ptr<IRaftStateMachine> machine;

        /// The running node; null while it is down, which only a refused `Restart` does.
        std::unique_ptr<RaftDriver> driver;

        /// The configuration this node was started with, empty for a joiner.
        ///
        /// Recorded per node rather than taken from the harness, because a
        /// restart has to reconstruct the configuration *this* node was given —
        /// and a joiner was given none, so reading the harness's list would hand
        /// it a bootstrap set on its second start that it never had on its first.
        Configuration bootstrap;

        /// Every entry handed to this node's application, in order, across restarts.
        ///
        /// A HISTORY of `Apply` calls, which is what State Machine Safety is checked
        /// against. It is not the application's state: a restart re-applies what the
        /// node recovers, so an entry can appear here more than once, and an entry a
        /// snapshot carried in never appears here at all.
        std::vector<AppliedEntry> applied;

        /// Whether this node's application can read no snapshot at all; see
        /// `SetSnapshotsUnreadable`.
        bool snapshotsUnreadable = false;

        /// What this node's application HOLDS: every entry it has applied or been
        /// restored with, in index order (#1542).
        ///
        /// The state the machine snapshots and restores, emptied by a restart the way
        /// a process's memory is. Before #1542 the harness kept no state at all and
        /// restored nothing, so every restart case passed whether or not a recovered
        /// snapshot reached the application -- a fake more permissive than the
        /// component it stands for. This is what a restart case asserts on.
        std::vector<AppliedEntry> application;
    };

  private:
    struct InFlight
    {
        NodeId from;
        NodeId to;
        RaftMessage message;
        core::platform::SteadyTimePoint deliverAt;
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

    /// A real state machine: it holds what it applied, snapshots it and restores it.
    ///
    /// Its state is `Member::application`, the ordered entries it has applied, and
    /// its snapshot is exactly that state encoded -- so a restore that never happens
    /// leaves a restarted node visibly missing what its snapshot covered, and a
    /// restore that merged instead of replacing would visibly keep what an installed
    /// snapshot superseded. Every `Apply` is also recorded in `Member::applied`, the
    /// history State Machine Safety is checked against.
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

        /// Every command: this harness's commands are a case's opaque bytes, in no
        /// format a build could fail to read. What an unreadable command does to a
        /// recovering node is asked over the production machine, in `RaftDriver_test`.
        [[nodiscard]] std::expected<void, ConsensusError> CanRead(std::span<std::byte const> command) const override
        {
            std::ignore = command;
            return {};
        }

        [[nodiscard]] std::expected<void, ConsensusError> CanRestore(std::span<std::byte const> state) const override
        {
            return _harness.CanRestoreApplication(_who, state);
        }

        [[nodiscard]] std::vector<std::byte> TakeSnapshot() override
        {
            return EncodeApplication(_harness.Find(_who).application);
        }

        [[nodiscard]] std::expected<void, ConsensusError> RestoreSnapshot(std::span<std::byte const> state,
                                                                          SnapshotOrigin origin) override
        {
            return _harness.RestoreApplication(_who, state, origin);
        }

      private:
        RaftClusterHarness& _harness;
        NodeId _who;
    };

    /// Encode an application's state as a snapshot: `[u64 index][payload]` per entry.
    /// @param application The entries, in index order.
    /// @return The bytes.
    [[nodiscard]] static std::vector<std::byte> EncodeApplication(std::vector<AppliedEntry> const& application);

    /// Read back what `EncodeApplication` wrote.
    /// @param state The snapshot's bytes.
    /// @return The entries, or nullopt for bytes it cannot have written.
    [[nodiscard]] static std::optional<std::vector<AppliedEntry>> DecodeApplication(std::span<std::byte const> state);

    /// Whether `who`'s application could take on `state`, without taking it on.
    ///
    /// Refused for bytes its encoder cannot have written, and for every snapshot while a
    /// case has made the member unable to read them (`SetSnapshotsUnreadable`).
    /// @param who The node.
    /// @param state A snapshot's bytes.
    /// @return Nothing when `RestoreApplication` would take it on; otherwise why not.
    [[nodiscard]] std::expected<void, ConsensusError> CanRestoreApplication(NodeId const& who,
                                                                            std::span<std::byte const> state) const;

    /// Replace `who`'s application state with the one `state` encodes, or refuse it.
    ///
    /// Wholesale, as `IRaftStateMachine::RestoreSnapshot` requires, and a refusal
    /// changes nothing. Bytes this harness's own encoder could not have produced are
    /// refused, never read as an empty state, because an empty state is exactly what a
    /// missed restore looks like and the two must not read alike. A LEADER's snapshot
    /// that will not decode is also a VIOLATION, since every leader here encodes with
    /// that encoder; a node's own recovered one is a case's doing, and its refusal is
    /// what that case asserts (#1542).
    /// @param who The node.
    /// @param state A snapshot `EncodeApplication` produced, on some node.
    /// @param origin Which of `RestoreSnapshot`'s two callers this is.
    /// @return Nothing once replaced; otherwise the refusal, as `StorageFailure`.
    [[nodiscard]] std::expected<void, ConsensusError> RestoreApplication(NodeId const& who,
                                                                         std::span<std::byte const> state,
                                                                         SnapshotOrigin origin);

    /// Build a driver for `member` over `node`, the one way this harness makes one.
    ///
    /// Shared by `AddNode` and `Restart`, so the compaction policy -- and whatever the
    /// next collaborator is -- cannot reach a first start and miss a restart. Through
    /// `RaftDriver::Create`, production's one seam, so a refusal there is one here.
    /// @param member The node's world; its driver is set only on success.
    /// @param node The Raft node the driver takes over.
    /// @return Nothing once built; otherwise `Create`'s refusal.
    [[nodiscard]] std::expected<void, ConsensusError> BuildDriver(Member& member, RaftNode node);

    /// The configuration every node in this cluster runs with.
    ///
    /// One definition rather than two identical literals: a restart has to
    /// reconstruct the same configuration the node started with, and a second copy
    /// is one that can drift -- a restarted node quietly running different
    /// timeouts would change what the simulation is testing without saying so.
    /// @param who Which member the configuration is for.
    /// @param bootstrap That member's own bootstrap configuration; empty for a joiner.
    /// @return The configuration.
    [[nodiscard]] static RaftConfig ConfigFor(NodeId const& who, Configuration bootstrap);

    /// Build one node's whole world and put it on the network.
    ///
    /// One definition of "a node in this harness", called by both the constructor
    /// and `Join`. Two copies would let the next per-node collaborator be wired
    /// into bootstrap nodes and silently absent from joiners — a difference in
    /// exactly the dimension this harness exists to compare.
    /// @param who The node's name on the network.
    /// @param bootstrap Its bootstrap configuration; empty for a joiner.
    /// @param identity Who it proves itself as.
    void AddNode(NodeId const& who, Configuration bootstrap, std::unique_ptr<IRaftPeerIdentity const> identity);

    /// Refuse a second node under an id already on the network.
    /// @param who The id about to be added.
    void RequireNew(NodeId const& who) const;

    /// Run the peer wire's exchange for one message, sender dialling receiver.
    ///
    /// Every step the production ends take, in their order, with their objects: a
    /// failure at any of them is a refusal, and only a message that decoded from a
    /// frame the receiver opened -- naming the sender that proved its id -- is
    /// delivered.
    /// @param message The message, with its sender and receiver.
    /// @return What the receiver decoded, or nullopt when the exchange refused it.
    [[nodiscard]] std::optional<RaftMessage> Authenticate(InFlight const& message);

    void Enqueue(NodeId const& from, NodeId const& to, RaftMessage message);
    void RecordApplied(NodeId const& who, AppliedEntry const& entry);
    [[nodiscard]] bool Reaches(NodeId const& from, NodeId const& to) const;
    void CheckInvariants();

    [[nodiscard]] Member& Find(NodeId const& who);

    /// Whether @p member is running. The one statement of the rule a refused `Restart`
    /// introduced -- a node that is down has no driver -- so the loops that must skip
    /// such a node filter on this rather than each testing the pointer.
    /// @param member The node.
    /// @return True unless it is down.
    [[nodiscard]] static bool IsUp(std::unique_ptr<Member> const& member) noexcept
    {
        return member->driver != nullptr;
    }

    core::platform::ManualClock _clock;

    /// Drives delay and loss decisions. Seeded fixed, so a failure is replayable
    /// from the same seed rather than being a flake to re-run.
    SystemRandomSource _network { 0x5EED };

    /// Every handshake nonce and ephemeral key. A source apart from `_network`, so
    /// authenticating a message draws nothing an existing seed's delays and losses depended
    /// on.
    ///
    /// The operating system's generator, the one production draws from (#1527), and
    /// NOT a seeded engine: that is what a nonce is now made of, and an engine here
    /// would be the one place left that still spelled a nonce the old way. Determinism
    /// does not suffer, because a nonce's VALUE decides nothing -- every signature over it
    /// verifies under the signer's key and under no other, and every session key agreed from
    /// it is shared by exactly the two ends, whatever the bytes are -- so a run is still a
    /// function of its seeds alone.
    SystemSecureRandom _nonces;

    /// Who each member and each plain `Join` is.
    IdentityFactory _identities;

    /// Refused deliveries, by receiver.
    std::map<NodeId, std::uint64_t> _refusedAt;

    /// Completed deliveries, by sender.
    std::map<NodeId, std::uint64_t> _deliveredFrom;

    /// The constructor's seed stagger, kept so `Join` can continue the same
    /// series rather than start a second one that could collide with it.
    std::uint64_t _seedOffset { 1 };

    /// When every node's driver trims its log; kept so a restart or a join runs
    /// with the policy the cluster was built with.
    CompactionPolicy _compaction {};

    /// The configuration the harness was constructed with: every bootstrap node,
    /// by the set it was started in.
    Configuration _members;
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

inline RaftClusterHarness::RaftClusterHarness(std::vector<NodeId> members,
                                              IdentityFactory identities,
                                              std::uint64_t seedOffset,
                                              CompactionPolicy compaction):
    RaftClusterHarness {
        Configuration { .voters = std::move(members), .learners = {} }, std::move(identities), seedOffset, compaction
    }
{
}

inline RaftClusterHarness::RaftClusterHarness(Configuration configuration,
                                              IdentityFactory identities,
                                              std::uint64_t seedOffset,
                                              CompactionPolicy compaction):
    _identities { std::move(identities) },
    _seedOffset { seedOffset },
    _compaction { compaction },
    _members { std::move(configuration) }
{
    // Voters first and then learners, which is the order the seed series is drawn
    // in: a cluster of voters alone gets exactly the seeds it got before learners
    // existed, so no existing case's schedule moved.
    for (auto const* const set: { &_members.voters, &_members.learners })
        for (auto const& id: *set)
            AddNode(id, _members, _identities(id));
}

inline void RaftClusterHarness::AddNode(NodeId const& who,
                                        Configuration bootstrap,
                                        std::unique_ptr<IRaftPeerIdentity const> identity)
{
    auto member = std::make_unique<Member>();
    member->id = who;
    member->identity = std::move(identity);
    member->storage = std::make_unique<InMemoryRaftStorage>();
    member->transport = std::make_unique<QueueingTransport>(*this, member->id);
    member->machine = std::make_unique<RecordingMachine>(*this, member->id);

    // A distinct seed per node, because the whole point of a randomized election
    // timeout is that two nodes do not draw the same one -- and a cluster whose
    // members all drew identically would split its vote, retry, and split it again
    // for as long as the test ran. Indexed by the node count rather than by a loop
    // variable, so a node joining later continues the same series instead of
    // starting a second one that could collide with it.
    member->random = std::make_unique<SystemRandomSource>((_seedOffset * 1000) + _nodes.size());
    member->bootstrap = std::move(bootstrap);

    auto node = RaftNode::Create(ConfigFor(member->identity->Self(), member->bootstrap), *member->random, _clock.now());

    // A node that has never run recovered nothing, so nothing can be refused; one that
    // was is this harness broken, not an outcome for a case to assert.
    if (auto built = BuildDriver(*member, std::move(node).value()); !built.has_value())
        throw std::logic_error { "a fresh node's driver was refused: " + built.error().context };

    _nodes.push_back(std::move(member));
}

inline std::expected<void, ConsensusError> RaftClusterHarness::BuildDriver(Member& member, RaftNode node)
{
    auto driver = RaftDriver::Create(std::move(node), *member.storage, *member.transport, *member.machine, _compaction);
    if (!driver.has_value())
        return std::unexpected { std::move(driver).error() };
    member.driver = *std::move(driver);
    return {};
}

inline std::vector<std::byte> RaftClusterHarness::EncodeApplication(std::vector<AppliedEntry> const& application)
{
    // The indices are written out before any span into them is taken, so the list
    // below never views storage a growing vector has moved.
    auto indices = std::vector<std::array<std::byte, sizeof(std::uint64_t)>> {};
    indices.reserve(application.size());
    for (auto const& entry: application)
        indices.push_back(WireFields::ToBigEndian<std::uint64_t>(entry.index.value));

    // Indexed rather than zipped: `std::views::zip` is not uniformly available across
    // the standard libraries this project builds against.
    auto fields = std::vector<std::span<std::byte const>> {};
    fields.reserve(application.size() * 2);
    for (auto const at: std::views::iota(std::size_t { 0 }, application.size()))
    {
        fields.emplace_back(indices[at]);
        fields.emplace_back(application[at].payload);
    }
    return WireFields::Encode(WireFields::FieldList { fields });
}

inline std::optional<std::vector<AppliedEntry>> RaftClusterHarness::DecodeApplication(std::span<std::byte const> state)
{
    auto const fields = WireFields::SplitAll(state);
    if (!fields.has_value() || fields->size() % 2 != 0)
        return std::nullopt;

    auto entries = std::vector<AppliedEntry> {};
    entries.reserve(fields->size() / 2);
    // Pairs by position rather than `std::views::chunk`, for `zip`'s reason above.
    for (auto const at: std::views::iota(std::size_t { 0 }, fields->size() / 2))
    {
        auto const index = WireFields::FromBigEndian<std::uint64_t>((*fields)[2 * at]);
        if (!index.has_value())
            return std::nullopt;
        auto const payload = (*fields)[(2 * at) + 1];
        entries.push_back(AppliedEntry { .index = LogIndex { .value = *index },
                                         .payload = std::vector<std::byte> { payload.begin(), payload.end() } });
    }
    return entries;
}

inline std::expected<void, ConsensusError> RaftClusterHarness::CanRestoreApplication(NodeId const& who,
                                                                                     std::span<std::byte const> state) const
{
    if (At(who).snapshotsUnreadable)
        return std::unexpected { UnsupportedFormatVersion(who + " reads no snapshot: a case made it so") };
    if (!DecodeApplication(state).has_value())
        return std::unexpected { StorageFailure(who + "'s encoder cannot have written these bytes") };
    return {};
}

inline void RaftClusterHarness::SetSnapshotsUnreadable(NodeId const& who, bool unreadable)
{
    Find(who).snapshotsUnreadable = unreadable;
}

inline std::expected<void, ConsensusError> RaftClusterHarness::RestoreApplication(NodeId const& who,
                                                                                  std::span<std::byte const> state,
                                                                                  SnapshotOrigin origin)
{
    // Refused exactly where `CanRestoreApplication` refuses, so the two cannot come to
    // disagree -- which the driver treats as fatal, and which would be this harness's doing.
    // An INSTALLED one refused here is a violation as well: the driver asked first, so it
    // should never have been handed one (#1552).
    if (auto readable = CanRestoreApplication(who, state); !readable.has_value())
    {
        auto refusal = std::move(readable).error();
        refusal.context = std::format("{}: {}", TraitsOf(origin).described, refusal.context);
        if (origin == SnapshotOrigin::Installed)
            _violations.push_back("Snapshot: " + refusal.context + ", and it was handed to the application anyway");
        return std::unexpected { std::move(refusal) };
    }

    // Replace, never merge -- the contract this harness exists to hold production to. It
    // decodes: `CanRestoreApplication` asked that very question above.
    Find(who).application = DecodeApplication(state).value_or(std::vector<AppliedEntry> {});
    return {};
}

inline RaftConfig RaftClusterHarness::ConfigFor(NodeId const& who, Configuration bootstrap)
{
    return RaftConfig { .self = who,
                        .voters = std::move(bootstrap.voters),
                        .learners = std::move(bootstrap.learners),
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

inline core::platform::SteadyTimePoint RaftClusterHarness::Now() const noexcept
{
    return _clock.now();
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

inline std::uint64_t RaftClusterHarness::RefusedAt(NodeId const& who) const
{
    auto const found = _refusedAt.find(who);
    return found == _refusedAt.end() ? 0 : found->second;
}

inline std::uint64_t RaftClusterHarness::DeliveredFrom(NodeId const& who) const
{
    auto const found = _deliveredFrom.find(who);
    return found == _deliveredFrom.end() ? 0 : found->second;
}

inline std::optional<RaftMessage> RaftClusterHarness::Authenticate(InFlight const& message)
{
    auto const& sender = Find(message.from);
    auto const& receiver = Find(message.to);

    // The receiver is the acceptor: it challenges before it has read anything. A nonce
    // that cannot be drawn is a delivery refused, as it is a connection closed on the wire.
    // The sender dials the id it ADDRESSED, which is the member it believes it is talking to.
    auto acceptorBegun = AcceptorHandshake::Create(*receiver.identity, _nonces);
    auto diallerBegun = DiallerHandshake::Create(*sender.identity, message.to, _nonces);
    if (!acceptorBegun.has_value() || !diallerBegun.has_value())
        return std::nullopt;
    auto& acceptor = *acceptorBegun;
    auto& dialler = *diallerBegun;

    auto const proof = dialler.Answer(acceptor.Challenge());
    if (!proof.has_value())
        return std::nullopt;

    auto judgement = acceptor.Judge(*proof);
    if (judgement.outcome != ProofOutcome::Accepted || !judgement.verdict.has_value() || !judgement.session.has_value())
        return std::nullopt;

    auto conclusion = dialler.Conclude(*judgement.verdict);
    if (conclusion.outcome != VerdictOutcome::Accepted || !conclusion.session.has_value())
        return std::nullopt;

    // Each end seals and opens under the key IT agreed, so a disagreement about the
    // session is a refused frame here exactly as it is on a connection.
    auto const frame = RaftWire::Encode(message.message);
    auto const bytes = std::span<std::byte const> { frame };
    auto const header = bytes.first(RaftWire::HeaderSize);
    auto const payload = bytes.subspan(RaftWire::HeaderSize);
    auto const tag = FrameSealer { *std::move(conclusion.session) }.Seal(header, payload);
    if (!FrameOpener { *std::move(judgement.session) }.Open(header, payload, tag))
        return std::nullopt;

    auto const decodedHeader = RaftWire::DecodeHeader(header);
    if (!decodedHeader.has_value())
        return std::nullopt;
    auto decoded = RaftWire::DecodeMessage(*decodedHeader, payload);
    if (!decoded.has_value() || SenderOf(*decoded) != judgement.dialler)
        return std::nullopt;

    return std::move(decoded).value();
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
                               .deliverAt = _clock.now() + std::chrono::milliseconds { delay * 5 } });
}

inline void RaftClusterHarness::RecordApplied(NodeId const& who, AppliedEntry const& entry)
{
    Find(who).applied.push_back(entry);
    Find(who).application.push_back(entry);

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
    // A node that is down holds no role and no log anybody can read.
    for (auto const& node: _nodes | std::views::filter(IsUp))
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
            //
            // "In the log" includes the part of it a snapshot replaced (#1542). Until
            // this harness compacted, no leader held one, and the check read the log
            // alone; a compacted leader holds an entry below its boundary only in its
            // snapshot -- which, now that the harness's application keeps real state,
            // says exactly which commands it covers, so the property is checked there
            // rather than waived. Decoded once per leader per step.
            auto const boundary = raft.SnapshotIndex().value;
            auto const covered = boundary == 0 ? std::optional { std::vector<AppliedEntry> {} }
                                               : DecodeApplication(raft.CurrentSnapshot().state);
            if (!covered.has_value())
                _violations.push_back("Snapshot: leader " + node->id
                                      + " holds a snapshot its own encoder cannot have written");

            for (auto const& [index, committed]: _appliedAt)
            {
                if (term <= committed.term)
                    continue;

                if (index <= boundary)
                {
                    auto const& snapshotted = covered.value_or(std::vector<AppliedEntry> {});
                    auto const found = std::ranges::find(snapshotted, LogIndex { .value = index }, &AppliedEntry::index);
                    if (found == snapshotted.end() || found->payload != committed.payload)
                        _violations.push_back("Leader Completeness: leader " + node->id + " of term " + std::to_string(term)
                                              + " has no snapshot of the entry committed at index " + std::to_string(index)
                                              + " in term " + std::to_string(committed.term));
                    continue;
                }

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
    auto up = std::vector<Member const*> {};
    for (auto const& node: _nodes | std::views::filter(IsUp))
        up.push_back(node.get());

    for (auto const outer: std::views::iota(std::size_t { 0 }, up.size()))
    {
        for (auto const inner: std::views::iota(outer + 1, up.size()))
        {
            auto const& left = up[outer]->driver->Node().Log();
            auto const& right = up[inner]->driver->Node().Log();
            auto const shared = std::min(left.LastIndex().value, right.LastIndex().value);

            // Descending to 1, so the range is built ascending and reversed -- the spelling
            // `DashboardPanel_test.cpp` already uses. Empty when `shared` is 0, which is what
            // `0 >= 1` gave.
            for (auto const index: std::views::iota(std::uint64_t { 1 }, shared + 1) | std::views::reverse)
            {
                auto const at = LogIndex { .value = index };
                if (left.TermAt(at) != right.TermAt(at))
                    continue;

                // Agreed here, so every entry below must be identical.
                for (auto const below: std::views::iota(std::uint64_t { 1 }, index + 1) | std::views::reverse)
                {
                    auto const* const a = left.EntryAt(LogIndex { .value = below });
                    auto const* const b = right.EntryAt(LogIndex { .value = below });
                    if (a != nullptr && b != nullptr && (a->term != b->term || a->payload != b->payload))
                    {
                        _violations.push_back("Log Matching: " + up[outer]->id + " and " + up[inner]->id + " agree at index "
                                              + std::to_string(index) + " but differ at " + std::to_string(below));
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
    _clock.advance(by);
    auto const now = _clock.now();

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

        auto authenticated = Authenticate(message);
        if (!authenticated.has_value())
        {
            ++_refusedAt[message.to];
            continue;
        }

        // A node that is down hears nothing, as a process that is not running does not.
        auto& receiver = Find(message.to);
        if (receiver.driver == nullptr)
            continue; // Down: see `IsUp`.

        ++_deliveredFrom[message.from];
        (void) receiver.driver->Receive(*std::move(authenticated), now);
    }

    for (auto& node: _nodes | std::views::filter(IsUp))
        (void) node->driver->Tick(now);

    CheckInvariants();
}

inline void RaftClusterHarness::Run(std::size_t steps, std::chrono::milliseconds by)
{
    for ([[maybe_unused]] auto const step: std::views::iota(std::size_t { 0 }, steps))
        Step(by);
}

inline std::vector<NodeId> RaftClusterHarness::Leaders() const
{
    auto found = std::vector<NodeId> {};
    for (auto const& node: _nodes | std::views::filter(IsUp))
        if (node->driver->Node().CurrentRole() == Role::Leader)
            found.push_back(node->id);

    return found;
}

inline std::optional<NodeId> RaftClusterHarness::Leader() const
{
    // The highest-term leader, not "the one leader", because more than one node
    // can truthfully report itself leader at the same moment. A leader cut off
    // from its peers does step down -- since issue #437 its own `Tick` relinquishes
    // leadership once `HasQuorumContact` fails -- but not instantly: it goes on
    // reporting itself leader for up to one `electionTimeoutMin` after the last
    // answer it had, and longer still for a member it has only just admitted, which
    // is owed that window before its silence counts (issue #1061). What this comment
    // used to say -- that stepping down on that answer is "a separate mechanism this
    // library does not have" -- was true when it was written and false for months
    // afterwards, which is what #1061 cost. There is still no leader lease. What
    // holds throughout is the guarantee that matters: the stale one cannot commit
    // anything, because committing needs a quorum it cannot reach.
    //
    // This is not a loophole in Election Safety, which is per *term*: the stale
    // leader holds an older term than the one the majority elected.
    auto best = std::optional<NodeId> {};
    auto bestTerm = Term::None();

    for (auto const& node: _nodes | std::views::filter(IsUp))
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

    auto const proposed = Find(*leader).driver->Propose(std::move(payload), _clock.now());
    if (!proposed.has_value())
        return std::nullopt;

    return *proposed;
}

inline std::optional<LogIndex> RaftClusterHarness::ProposeMembershipOnLeader(Configuration configuration)
{
    auto const leader = Leader();
    if (!leader.has_value())
        return std::nullopt;

    auto const proposed = Find(*leader).driver->ProposeMembership(std::move(configuration), _clock.now());
    if (!proposed.has_value())
        return std::nullopt;

    return *proposed;
}

inline std::expected<void, ConsensusError> RaftClusterHarness::Restart(NodeId const& who)
{
    auto& member = Find(who);

    // The old process is gone before the new one starts, whether or not it does: a node
    // whose restart is refused is DOWN, never still running the driver it had.
    member.driver.reset();

    // Forgotten before the new driver exists, as a process's memory is: whatever the
    // application holds from here on, recovery put there.
    member.application.clear();

    // A store that cannot be read is a refused start as well, and says so; it used to
    // return here silently, leaving the old driver running as if it had restarted.
    auto recovered = member.storage->Load();
    if (!recovered.has_value())
        return std::unexpected { std::move(recovered).error() };

    auto node = RaftNode::Create(
        ConfigFor(member.identity->Self(), member.bootstrap), *member.random, _clock.now(), std::move(recovered).value());
    if (!node.has_value())
        return std::unexpected { std::move(node).error() };
    return BuildDriver(member, std::move(node).value());
}

inline void RaftClusterHarness::RequireNew(NodeId const& who) const
{
    for (auto const& node: _nodes)
        if (node->id == who)
            throw std::invalid_argument { "cluster member already exists: " + who };
}

inline void RaftClusterHarness::Join(NodeId const& who)
{
    Join(who, _identities(who));
}

inline void RaftClusterHarness::Join(NodeId const& who, std::unique_ptr<IRaftPeerIdentity const> identity)
{
    RequireNew(who);

    // The same construction every bootstrap node gets, differing in exactly one
    // thing: no bootstrap set. Anything else that differed would be a difference in
    // the very dimension this harness exists to compare.
    AddNode(who, {}, std::move(identity));
}

inline void RaftClusterHarness::Intrude(NodeId const& who, std::unique_ptr<IRaftPeerIdentity const> identity)
{
    RequireNew(who);

    // The id it CLAIMS joins its bootstrap set -- unless it is a member's, which is already
    // there, and a configuration naming one id twice is not one `RaftNode::Create` accepts.
    auto bootstrap = _members;
    if (std::ranges::find(bootstrap.voters, identity->Self()) == bootstrap.voters.end())
        bootstrap.voters.push_back(identity->Self());
    AddNode(who, std::move(bootstrap), std::move(identity));
}

} // namespace FastCache::Consensus
