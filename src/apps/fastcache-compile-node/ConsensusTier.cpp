// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "NodeFormation.hpp"
#include "NodeIdentity.hpp"
#include "NodeStateFiles.hpp"
#include "NodeSurfaces.hpp"
#include "SchedulingRedirect.hpp"

#include <FastCache/Cluster/Roster.hpp>
#include <FastCache/Consensus/RaftMembership.hpp>
#include <FastCache/Consensus/RaftNode.hpp>
#include <FastCache/Core/Ed25519.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/StopAwareWait.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <core/net/IConnector.hpp>
#include <core/net/PlatformLoop.hpp>
#include <core/net/Sockets.hpp>
#include <core/platform/Clock.hpp>

namespace FastCache::Node
{

namespace
{
    /// Hands what the peer server decoded to the driver.
    ///
    /// A shim rather than making `RaftDriver` an `IRaftMessageSink` itself: the sink
    /// is called from a peer-reader coroutine and `Receive` needs a timestamp, so
    /// this is where the two vocabularies meet. Keeping it here means the driver's
    /// own interface says nothing about who calls it or when -- which matters,
    /// because that reader shares the reactor thread with the driver's own tick
    /// loop and advances the node while that loop sits parked in the timer wheel.
    class DriverSink final: public Consensus::IRaftMessageSink
    {
      public:
        /// @param driver Where messages go; must outlive this.
        /// @param logger Where a rejected message is reported; must outlive this.
        DriverSink(Consensus::RaftDriver& driver, ILogger& logger) noexcept:
            _driver { driver },
            _logger { logger }
        {
        }

        void Deliver(Consensus::RaftMessage message) override
        {
            // `steady_clock` rather than the reactor's clock, and they are the same
            // clock: `core::net::PlatformLoop` reports steady time, so a message stamped here
            // and a timer fired there are on one timeline. Reading a wall clock
            // instead would let an NTP step look like an election timeout.
            auto const now = std::chrono::steady_clock::now();
            if (auto const applied = _driver.Receive(message, now); !applied.has_value())
                // Logged and dropped, never fatal. A message this node could not
                // apply is one peer's problem; stopping here would make it every
                // peer's, and a node that removed itself from a healthy cluster is a
                // partition it created for itself.
                _logger.Logf(LogLevel::Warn, "consensus: a peer message was refused: {}", applied.error().context);
        }

      private:
        Consensus::RaftDriver& _driver;
        ILogger& _logger;
    };

    /// One row per role: the role, and what a log line calls it.
    struct RoleNameRow
    {
        Distributed::SchedulerRole role; ///< The role this row names.
        std::string_view name;           ///< What a log line calls it.
    };

    /// What to call a role in a log line.
    ///
    /// A table rather than a conditional chain, which is what clang-tidy's
    /// `readability-avoid-nested-conditional-operator` is really asking for and what
    /// this codebase asks for anyway: a fourth role is a row here rather than another
    /// `?:` somebody threads through an existing expression.
    ///
    /// The row carries the role it names rather than leaving it to a trailing
    /// comment, which is what lets the order be checked at all: a bare array of
    /// names can only have its length asserted, and a length asserted against the
    /// last enumerator by name is the guard that fires only when nothing is wrong.
    constexpr EnumTable<Distributed::SchedulerRole, RoleNameRow> RoleNames { {
        { .role = Distributed::SchedulerRole::Follower, .name = "a follower" },
        { .role = Distributed::SchedulerRole::Undecided, .name = "undecided" },
        { .role = Distributed::SchedulerRole::Leader, .name = "the leader" },
    } };

    static_assert(RowsInEnumeratorOrder(RoleNames, &RoleNameRow::role),
                  "RoleNames must hold one row per SchedulerRole, in enumerator order");

    /// @param role The role.
    /// @return Its name.
    [[nodiscard]] constexpr std::string_view RoleName(Distributed::SchedulerRole role) noexcept
    {
        return RoleNames[static_cast<std::size_t>(role)].name;
    }

    /// One member's address, as something the transport can dial.
    ///
    /// The pair `SplitHostPort` and `ParseTcpPort` make, in the one place: the two
    /// callers here turn the same text into the same thing at startup and again on
    /// every reconcile pass, and a second spelling would let a bare port or a
    /// bracketed v6 address be dialable in one of them and not the other.
    /// @param id Whose address it is.
    /// @param endpoint The `host:port` text.
    /// @return The peer, or nullopt when the text names none.
    [[nodiscard]] std::optional<Consensus::PeerEndpoint> PeerEndpointFor(Consensus::NodeId const& id,
                                                                         std::string_view endpoint)
    {
        auto const split = SplitHostPort(endpoint);
        if (!split.has_value())
            return std::nullopt;

        auto const port = ParseTcpPort(split->second);
        if (!port.has_value())
            return std::nullopt;

        return Consensus::PeerEndpoint { .id = id, .host = split->first, .port = *port };
    }

    /// This node's role, in the scheduler's vocabulary.
    ///
    /// Three roles collapse to two here and the third is the interesting one.
    /// `PreCandidate` and `Candidate` both mean *nobody is known to lead*, which is
    /// `Undecided` rather than `Follower`: a follower can name a leader to redirect
    /// to, and a candidate cannot, and answering `NotLeader` with an empty endpoint
    /// is exactly what `Undecided` exists to express.
    /// @param role What consensus says.
    /// @param knownLeader Who it believes leads, if anybody.
    /// @return The scheduler's role.
    [[nodiscard]] Distributed::SchedulerRole SchedulerRoleFor(Consensus::Role role,
                                                              std::optional<Consensus::NodeId> const& knownLeader)
    {
        if (role == Consensus::Role::Leader)
            return Distributed::SchedulerRole::Leader;
        if (knownLeader.has_value())
            return Distributed::SchedulerRole::Follower;
        return Distributed::SchedulerRole::Undecided;
    }
} // namespace

ConsensusStatus ConsensusStatusFrom(Consensus::RaftDriver::Progress const& progress)
{
    return ConsensusStatus { .configuration = progress.configuration,
                             .knownLeader = progress.knownLeader,
                             .term = progress.term,
                             .commitIndex = progress.commitIndex,
                             .role = progress.role };
}

std::string DescribeRole(Distributed::SchedulerRole role, Consensus::Term term, std::string_view leaderEndpoint)
{
    return std::format("consensus: this node is now {} in term {}{}",
                       RoleName(role),
                       term.value,
                       leaderEndpoint.empty() ? std::string {} : std::format(" of {}", leaderEndpoint));
}

std::string DescribeTermAdoption(Consensus::Term adopted, Consensus::TermAdoption const& cause)
{
    return std::format("consensus: term {} arrived from {}; this node was {} in term {}",
                       adopted.value,
                       cause.from,
                       Consensus::TraitsOf(cause.previousRole).name,
                       cause.previousTerm.value);
}

std::string UnreadableConsensusStateRefusal(std::filesystem::path const& directory, ConsensusError const& refusal)
{
    // Which of the two the application found decides the middle clause: another build's
    // layout is intact and has no conversion, while bytes no build wrote are damage -- a
    // disk or a crash to look at before anything is moved. The remedy after it is the
    // same either way, because either way this directory cannot be run on.
    auto const intact = refusal.code == ConsensusErrorCode::UnsupportedFormatVersion;
    return std::format("cannot start consensus on the state in {}: {}; {}: {}",
                       directory.string(),
                       refusal.context,
                       intact ? "it is intact, and there is no conversion"
                              : "it is damaged, which is a disk or a crash to investigate first",
                       Consensus::UnreadableStateRemedy);
}

std::string DescribeInstallRefusal(Consensus::RaftDriver::InstallRefusal const& refusal)
{
    return std::format("leader {} offered a snapshot as of log entry {} that this build cannot read ({}); this node "
                       "stays behind it and follows no change the cluster makes until it can",
                       refusal.leader,
                       refusal.index.value,
                       refusal.reason.context);
}

void ReportInstallRefusal(std::optional<Consensus::RaftDriver::InstallRefusal> const& refusal,
                          ILogger& logger,
                          NodeConditions* conditions)
{
    if (!refusal.has_value())
    {
        logger.Log(LogLevel::Info,
                   "consensus: this node has caught up past the leader's snapshot it could not read, and follows its "
                   "cluster again");
        if (conditions != nullptr)
            conditions->Clear(NodeCondition::UnreadableLeaderSnapshot);
        return;
    }

    auto const said = DescribeInstallRefusal(*refusal);
    logger.Logf(LogLevel::Error, "consensus: {}", said);
    if (conditions != nullptr)
        conditions->Raise(NodeCondition::UnreadableLeaderSnapshot, said);
}

std::expected<Cluster::ClusterMember, std::string> ConsensusSelfMemberOf(NodeConfig const& cfg,
                                                                         std::span<Cluster::MemberSpec const> members,
                                                                         Ed25519PublicKey const& publicKey)
{
    if (cfg.nodeId.empty())
        return std::unexpected { std::string { ConsensusNeedsNodeIdRefusal } };

    // A mode that dials in is its recorded entry whatever it holds: its endpoint is one nobody
    // dials, so an empty one is the ordinary case rather than a gap.
    auto dial = ConsensusDialAddressOf(cfg);
    auto const dialsIn = !dial.has_value() && dial.error() == ConsensusDialGap::DialsIn;
    auto const* const named = core::findIfOrNull(members, [&cfg, dialsIn](Cluster::MemberSpec const& member) {
        return member.id == cfg.nodeId && (dialsIn || !member.raftEndpoint.empty());
    });

    if (named == nullptr && !dialsIn && !dial.has_value())
        return std::unexpected { std::string { ConsensusNamesNoDialAddressRefusal } };

    // A mode nobody dials holds a learner's seat, the one seat that needs no endpoint
    // (`Cluster::SeatNeedsEndpoint`); every other is the voter it founded or was promoted to --
    // which is the seat the approved roster records for this node too, since the mode follows it.
    // The key is the one this node proves itself with: a record always holds one.
    return Cluster::ClusterMember { .id = cfg.nodeId,
                                    .raftEndpoint = named != nullptr ? named->raftEndpoint : dial.value_or(std::string {}),
                                    .schedulerEndpoint = {},
                                    .schedulerEndpointHistory = Cluster::SchedulerEndpointHistory::NeverAnnounced,
                                    .seat = dialsIn ? Cluster::MemberSeat::Learner : Cluster::MemberSeat::Voter,
                                    .publicKey = publicKey };
}

ConsensusTierShape ConsensusTierShapeOf(NodeConfig const& cfg) noexcept
{
    if (!cfg.formation.has_value())
        return ConsensusTierShape {};
    auto const& row = Cluster::NodeModeRowFor(cfg.formation->mode);
    return ConsensusTierShape { .listens = row.raftListener == Cluster::RaftListenerState::Open, .direction = row.dials };
}

std::string DescribeConsensusEndpoint(std::string_view raftEndpoint)
{
    return raftEndpoint.empty() ? std::string { "with no consensus endpoint" } : std::format("at {}", raftEndpoint);
}

std::string OwnRaftEndpoint(Cluster::MemberSeat seat, std::string_view raftAdvertised)
{
    return Cluster::SeatNeedsEndpoint(seat) ? PeerDialableOrNone(raftAdvertised) : std::string {};
}

Cluster::DesiredMember SelfDesire(Cluster::DesiredMember held,
                                  Cluster::MemberSeat seat,
                                  std::string_view advertised,
                                  std::string_view raftAdvertised)
{
    held.schedulerEndpoint = PeerDialableOrNone(advertised);
    if (auto raft = OwnRaftEndpoint(seat, raftAdvertised); !raft.empty())
        held.raftEndpoint = std::move(raft);
    return held;
}

std::vector<Consensus::NodeId> DialInPeers(Cluster::ClusterState const& state, Consensus::Configuration const& configuration)
{
    auto peers = std::vector<Consensus::NodeId> {};
    for (auto const& member: state.members)
        if (Cluster::LinkOfSeat(member.seat) == Consensus::PeerLink::DialsIn)
            peers.push_back(member.id);
    for (auto const& row: Cluster::MemberSeatTable)
        for (auto const& id: configuration.*row.set)
            if (Consensus::TraitsOf(Consensus::Membership::StandingOf(configuration, id)).link
                == Consensus::PeerLink::DialsIn)
                peers.push_back(id);
    std::ranges::sort(peers);
    auto const duplicates = std::ranges::unique(peers);
    peers.erase(duplicates.begin(), duplicates.end());
    return peers;
}

Distributed::LeaderReading LeaderReadingOf(Consensus::RaftDriver::Progress const& progress,
                                           core::platform::SteadyTimePoint now)
{
    // A leader hears from no leader: while it leads, CheckQuorum is its evidence and deposes it
    // once a quorum stops answering, so leading IS contact, at this instant.
    auto const leads = progress.role == Consensus::Role::Leader;
    // NOT judged here against `progress.configuration`: that is the driver's ACTIVE configuration,
    // which an uncommitted entry can move. Whether the leader counts is the roster's question,
    // asked of the voters it applied (`Distributed::StateLeaseRoster::NoteLeaderReading`).
    // An AGE on this tier's clock, for the roster to take back from its own (`LeaderReading`).
    auto silentFor = std::optional<core::platform::SteadyTimePoint::duration> {};
    if (leads)
        silentFor = core::platform::SteadyTimePoint::duration::zero();
    else if (progress.lastLeaderContact.has_value())
        silentFor = now - *progress.lastLeaderContact;
    return Distributed::LeaderReading { .leads = leads, .leader = progress.knownLeader, .silentFor = silentFor };
}

ConsensusTier::ConsensusTier(Cluster::ClusterMember self,
                             Cc::IAdvertisedEndpointSource const& advertised,
                             Cc::IAdvertisedEndpointSource const& raftAdvertised,
                             Consensus::FileRaftStorage storage,
                             Ed25519KeyPair identityKey,
                             std::span<Cluster::MemberSpec const> knownMembers,
                             std::string boundEndpoint,
                             RoleObserver onRole,
                             MembersObserver onMembers,
                             LeaderContactObserver onLeaderContact,
                             std::string clusterId,
                             IMetricsSink& metrics,
                             ILogger& logger,
                             NodeConditions* conditions,
                             FormationHooks hooks):
    _logger { logger },
    _storage { std::move(storage) },
    // Unseeded, so two nodes started together do not draw the same election timeout and
    // split the vote round after round. The seeded constructor exists so a failure can be
    // replayed, and nothing replays a production node. Handshake nonces are `_nonces`'s.
    _random { std::make_unique<SystemRandomSource>() },
    _metrics { metrics },
    _roster { std::move(identityKey), knownMembers },
    _identity { self.id, _roster },
    _connector { core::net::makeConnector(_reactor, _resolver) },
    _application { logger, [this](Cluster::ClusterState const& state) { OnStateChanged(state); } },
    _onRole { std::move(onRole) },
    _boundEndpoint { std::move(boundEndpoint) },
    _self { std::move(self) },
    _advertised { advertised },
    _raftAdvertised { raftAdvertised },
    _onMembers { std::move(onMembers) },
    _onLeaderContact { std::move(onLeaderContact) },
    _conditions { conditions },
    _clusterId { std::move(clusterId) },
    _hooks { std::move(hooks) }
{
    // Seeded with this node's own record, and its scheduler endpoint travels as a
    // value that is PRESENT even when it is empty. That is an assertion -- "I know
    // what mine is" -- where a peer discovered on the segment has no opinion at
    // all, and `DesiredMember` keeps the two apart precisely so one cannot clear
    // what the other announced.
    //
    // No SEAT, and no desire carries one (#1449, #1535): which set a node is in is the
    // operator's decision, recorded by `--cluster-admit` and `--cluster-admit-learner`,
    // and a node that asserted its own on every pass would undo a demotion one interval
    // after it committed. Before its first pass it is recorded where the configuration
    // already counts it.
    //
    // The KEY is the scheduler endpoint's way round again (#178): a node is the authority
    // on the key it holds, read out of its own state directory, and nobody else can say
    // it. Absent on a node that holds none, which is no opinion rather than a claim.
    _desired.push_back(Cluster::DesiredMember { .id = _self.id,
                                                .raftEndpoint = _self.raftEndpoint,
                                                .schedulerEndpoint = _self.schedulerEndpoint,
                                                .publicKey = _self.publicKey });
}

std::expected<std::unique_ptr<ConsensusTier>, NodeRefusal> ConsensusTier::Start(
    NodeConfig const& cfg,
    Cc::IAdvertisedEndpointSource const& advertised,
    Cc::IAdvertisedEndpointSource const& raftAdvertised,
    std::optional<Ed25519KeyPair> const& identityKey,
    RoleObserver onRole,
    MembersObserver onMembers,
    LeaderContactObserver onLeaderContact,
    IMetricsSink& metrics,
    ILogger& logger,
    NodeConditions* conditions,
    FormationHooks hooks,
    std::unique_ptr<BlockingListener> boundListener)
{
    // The members the FORMATION starts consensus with (`BootstrapMembersOf`): this node alone
    // where it runs its own cluster, the fleet's roster where it joined one -- never both. No
    // flag carries them any more: the record decides, and a flag would be a second author that
    // could disagree with it.
    auto const members = BootstrapMembersOf(cfg);

    // The identity key, before anything is bound or dialled (#178). Every peer connection
    // proves each end's OWN key, so there is no unauthenticated consensus to fall back to --
    // #1308's rule, carried from the pre-shared key to the key that replaced it on this wire.
    // Every node has a state directory (`NodeStateDirectory`) and the start resolves
    // the key there before this tier exists, so this is the answer to a caller that did not.
    if (!identityKey.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, std::string { ConsensusNeedsIdentityKeyRefusal }) };

    // This node's own record (`ConsensusSelfMemberOf`). A node whose id names no member it can be
    // reached at could never win a vote and could never be voted for: it would stand for election
    // forever against a cluster that has never heard of it, which from the outside is a node that
    // simply never becomes ready.
    auto self = ConsensusSelfMemberOf(cfg, members, identityKey->PublicKey());
    // Both of its refusals are startup-table rows (`ConsensusNeedsNodeIdRefusal`,
    // `ConsensusNamesNoDialAddressRefusal`), asked before any tier: this is their belt.
    if (!self.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, std::move(self).error()) };

    // Only a node that FOUNDED its cluster bootstraps it. One that joined another's starts with
    // an empty bootstrap set and waits to be admitted -- the only shape a cluster can admit,
    // because a node that bootstrapped itself has elected itself and afterwards refuses every
    // leader its own configuration does not name.
    //
    // It still dials every member, and that is not an optimization: the leader admitting a
    // joiner starts replicating at its own last index, the joiner's log is empty, and the leader
    // only walks back to the beginning when the joiner REFUSES. A joiner that could not send
    // that refusal is admitted, dialled, and permanently silent.
    auto const foundedHere = cfg.formation.has_value() && cfg.formation->foundedHere;
    auto const bootstrap = foundedHere ? members : std::vector<Cluster::MemberSpec> {};

    // Who this node DIALS is every member whose seat is dialled (`Cluster::LinkOfSeat`), never a
    // learner: a learner dials in, so its recorded endpoint is empty or stale and either way nobody's
    // to dial -- and an empty one used to refuse the start of every voter whose fleet had a learner.
    // Every member still lends its KEY to the roster below, a learner's included: a voter must accept
    // the proof of the learner that dials it before it has applied any state.
    //
    // The bootstrap members are `MemberSpec`s, which carry no seat, so each one's seat is read
    // from the formation record they came from (`SeatInFormation`).
    auto dialable = std::vector<Cluster::MemberSpec> {};
    std::ranges::copy_if(members, std::back_inserter(dialable), [&cfg](Cluster::MemberSpec const& member) {
        return Cluster::LinkOfSeat(SeatInFormation(cfg, member.id)) == Consensus::PeerLink::Dialled;
    });

    // The mode's ROW decides the shape (`ConsensusTierShapeOf`): whether the Raft port is bound,
    // and which way the sessions this node dials flow.
    auto const shape = ConsensusTierShapeOf(cfg);

    // The wildcard for a bare port, like the scheduler's and unlike the cache's:
    // peers are on other machines by definition, so a loopback default would be one
    // that silently cannot work.
    //
    // `StartupPolicyRejection` asks this same question, so an operator meets it at
    // the command line -- an install included -- rather than here. What survives is
    // the message that names the text they typed, which a row of static prose
    // cannot, and the answer for a `NodeConfig` no argv produced (#168).
    // Through the surface's row, so the wildcard a bare port takes here is the same
    // value `--print-surfaces` prints and the same one the startup grammar judged.
    // The asymmetry with a worker's `--listen-node` loopback is the rule -- peers are on
    // other machines by definition -- and it is a column rather than a constant each
    // opener reaches for.
    //
    // Only for a mode whose row binds one: a learner listens for nobody, and its Raft surface
    // resolves no address by design -- asked anyway, that was a refusal of every learner's start.
    // A listener handed over for such a mode would be served by nothing, so it is refused by name
    // rather than closed in silence.
    auto bind = std::optional<SurfaceEndpoint> {};
    if (shape.listens)
    {
        auto resolved = SoleEndpointOf(NodeSurface::Raft, cfg);
        if (!resolved.has_value())
            return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, resolved.error()) };
        bind = *std::move(resolved);
    }
    else if (boundListener != nullptr)
        return std::unexpected { Refusal(NodeRefusalCause::HandedOverListeners,
                                         "a listener was handed to consensus, and this node's mode binds no "
                                         "consensus port") };

    // The listener is bound in `Launch` rather than here, because it binds against
    // the reactor and the reactor is a member of the object this has not built yet.
    // The refusal still reaches the operator: `Launch`'s error is this function's.

    // `NodeStateDirectory`, not a second spelling of the default: the identity is
    // recorded in this same directory since #1024, so a default written twice would be
    // a node reading its identity out of one directory and its log out of another. The
    // `<node-id>` suffix went with that change -- an identity read FROM the directory
    // cannot name it -- and what the suffix bought is unchanged, because two nodes on
    // one machine need two Raft logs and so two directories whatever they are called.
    auto const stateDirectory = NodeStateDirectory(cfg);
    auto storage = Consensus::FileRaftStorage::Open(stateDirectory);
    if (!storage.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::ConsensusStore,
                                         std::format("cannot open {}: {}{}",
                                                     stateDirectory.string(),
                                                     storage.error().context,
                                                     StateFileUnreadableHint(stateDirectory))) };

    // The record this node announces about itself, and the only place both of its
    // addresses are known at once: the consensus one is what an operator typed and
    // every peer dials, and the scheduler one is a port nobody connects to and so
    // nobody could otherwise learn.
    //
    // Its key is the one it proves itself with, and nobody else can state that: the member
    // entry `ApplyNodeIdentity` synthesised carries the same key, but the pair is the source.
    // Its seat is the one `ConsensusSelfMemberOf` read off its mode -- a learner announces a
    // learner, never the voter a bootstrap set of voters would have implied.
    //
    // Never one only this machine reaches (`PeerDialableOrNone`): a record is what OTHER machines
    // are sent to, and `Cluster::Validate` refuses such an endpoint on every route into it.
    auto announced = *self;
    announced.schedulerEndpoint = PeerDialableOrNone(advertised.Current());
    announced.publicKey = identityKey->PublicKey();

    auto tier = std::unique_ptr<ConsensusTier> { new ConsensusTier {
        std::move(announced),
        advertised,
        raftAdvertised,
        *std::move(storage),
        *identityKey,
        members,
        bind.has_value() ? std::format("{}:{}", bind->host, bind->port) : std::string {},
        std::move(onRole),
        std::move(onMembers),
        std::move(onLeaderContact),
        cfg.clusterId,
        metrics,
        logger,
        conditions,
        std::move(hooks) } };

    if (auto started = tier->Launch(cfg, dialable, bootstrap, bind, shape.direction, std::move(boundListener));
        !started.has_value())
        return std::unexpected { std::move(started).error() };

    logger.Logf(LogLevel::Info,
                "consensus {} as {} ({}, state in {})",
                bind.has_value() ? std::format("on {}", tier->BoundEndpoint())
                                 : std::format("dialling {} voter(s) and listening for nobody", dialable.size()),
                cfg.nodeId,
                bootstrap.empty() ? std::string { "no cluster yet; waiting to be admitted" }
                                  : std::format("{} member(s)", bootstrap.size()),
                stateDirectory.string());
    return tier;
}

std::expected<void, NodeRefusal> ConsensusTier::Listen(SurfaceEndpoint const& bind,
                                                       std::unique_ptr<BlockingListener> boundListener)
{
    // A socket the caller already bound is served as it is, and only if it is the ADDRESS the
    // configuration names -- host and port both: every peer dials `--raft-peer`'s address for
    // this node, so a listener on another port, or on another interface of the right port, is
    // a node nobody can reach that reports itself up. Adopted onto THIS node's reactor, for the
    // reason the bind below is.
    if (boundListener != nullptr)
    {
        if (!boundListener->IsBound())
            return std::unexpected { Refusal(
                NodeRefusalCause::HandedOverListeners,
                std::format("the listener handed to consensus is not bound: {}", boundListener->BindError())) };
        // The host is compared as the KERNEL reports it, against the configured host's canonical
        // literal. A configured NAME would need a lookup this path does not make, so it is refused
        // rather than matched by spelling; an empty host is the wildcard, which either family's
        // unspecified address answers.
        auto const heldHost = boundListener->BoundAddress();
        auto const heldPort = boundListener->boundPort();
        auto const configuredHost = CanonicalAddressLiteral(bind.host);
        if (!bind.host.empty() && !configuredHost.has_value())
            return std::unexpected { Refusal(
                NodeRefusalCause::HandedOverListeners,
                std::format("the listener handed to consensus cannot be matched to {}: the configuration names its "
                            "host by name, and a handed listener is matched against an address literal only",
                            FormatHostPort(bind.host, bind.port))) };
        auto const hostMatches =
            bind.host.empty() ? (heldHost == "0.0.0.0" || heldHost == "::") : heldHost == *configuredHost;
        if (!hostMatches || heldPort != bind.port)
            return std::unexpected { Refusal(NodeRefusalCause::HandedOverListeners,
                                             std::format("the listener handed to consensus is bound to {}, and the "
                                                         "configuration names {}",
                                                         FormatHostPort(heldHost, heldPort),
                                                         FormatHostPort(bind.host, bind.port))) };
        auto adopted = AdoptBoundListener(_reactor, boundListener->Release());
        if (!adopted.has_value())
            return std::unexpected { Refusal(NodeRefusalCause::Listener,
                                             std::format("cannot serve the listener handed to consensus on {}: {}",
                                                         FormatHostPort(bind.host, bind.port),
                                                         adopted.error())) };
        _listener = std::move(*adopted);
    }
    else
    {
        // Bound against THIS node's reactor, which is what makes `co_await Accept()`
        // and every read inside `RaftPeerServer` actually suspend. A blocking listener
        // would serve the first peer that connects and never accept another.
        auto listened = core::net::listen(_reactor, core::net::ListenOptions { .host = bind.host, .port = bind.port });

        // The failure is the `expected`'s, and carries its own diagnostic.
        if (!listened.has_value())
        {
            // Through the row (#352), which carries why this is fatal. Not restated
            // here: a paraphrase beside a pointer is two copies that can disagree.
            auto judged = JudgeBindFailure(
                RowFor(NodeSurface::Raft),
                std::format("cannot bind {}: {}", FormatHostPort(bind.host, bind.port), listened.error().toString()),
                _logger);
            if (!judged.has_value())
                return std::unexpected { Refusal(NodeRefusalCause::Listener, std::move(judged).error()) };

            // Refused rather than tolerated (#352). This is the earliest point in `Launch`,
            // so returning success here hands `Start` a tier whose `_transport`, `_driver`,
            // `_sink` and `_peerServer` were never built -- it would log "consensus on ..."
            // against a listener that never bound, and the first `Propose` would dereference
            // a null `_driver`. Carrying a tolerated verdict here is not a branch, it is the
            // rest of this function.
            return std::unexpected { Refusal(NodeRefusalCause::Listener,
                                             BindToleranceUnsupported(RowFor(NodeSurface::Raft),
                                                                      "the tier's driver, transport and peer server are "
                                                                      "built below this point")) };
        }
        _listener = std::move(*listened);
    }
    return {};
}

std::expected<void, NodeRefusal> ConsensusTier::Launch(NodeConfig const& cfg,
                                                       std::vector<Cluster::MemberSpec> const& dialable,
                                                       std::vector<Cluster::MemberSpec> const& bootstrap,
                                                       std::optional<SurfaceEndpoint> const& bind,
                                                       Consensus::RaftWire::SessionDirection direction,
                                                       std::unique_ptr<BlockingListener> boundListener)
{
    // FIRST, so a port that cannot be bound refuses before anything is built over it. A mode whose
    // row binds none has no listener, and no peer server below either.
    if (bind.has_value())
        if (auto listened = Listen(*bind, std::move(boundListener)); !listened.has_value())
            return listened;

    // Two lists out of two, and the split is the whole of how a node joins. Who
    // this node DIALS is everything its operator named; who consensus COUNTS is
    // the bootstrap set, which is empty for a node waiting to be admitted. A
    // joiner that dialled only itself could never answer the leader that admits
    // it -- and it cannot learn that leader's address from the replicated state,
    // because receiving the state is what answering makes possible.
    std::vector<Consensus::PeerEndpoint> peers;
    peers.reserve(dialable.size());
    for (auto const& member: dialable)
    {
        // Already checked by `ParseMemberSpec`, so this cannot fail -- but it is
        // split again rather than carried, because carrying it would mean the parsed
        // form and the string could disagree about which of a v6 address's colons is
        // the port separator, which is the defect `Core/HostPort` exists to hold in
        // one place.
        auto where = PeerEndpointFor(member.id, member.raftEndpoint);
        if (!where.has_value())
            return std::unexpected { Refusal(
                NodeRefusalCause::EarlierRule,
                std::format("{} is not a dialable endpoint for {}", member.raftEndpoint, member.id)) };

        // This node itself is deliberately included. `RaftPeerTransport` refuses a
        // message addressed to `self` rather than looping it through a socket, so
        // filtering here would duplicate a rule it already enforces -- and doing it
        // in two places is how they come to disagree about which node is which.
        peers.push_back(*std::move(where));
    }

    _bootstrapIds.reserve(bootstrap.size());
    for (auto const& member: bootstrap)
        _bootstrapIds.push_back(member.id);

    // A copy rather than moving `_bootstrapIds` into the configuration: the member
    // is what the reconciler compares against for the whole life of this node, and
    // `RaftConfig` owns its own list from here.
    //
    // Every one of them a VOTER (#1449). A learner is admitted at runtime -- by
    // `--cluster-admit-learner`, into the replicated record -- and never typed into a
    // bootstrap set, which every member must agree on and a flag cannot say which
    // seat of.
    auto ids = _bootstrapIds;

    // Which way every session this node dials flows is the mode row's `dials` column: `TwoWay` for a
    // learner, which nobody dials, so the voter it dials answers on the same connection.
    auto options = Consensus::PeerTransportOptions {};
    options.direction = direction;
    _transport = std::make_unique<Consensus::RaftPeerTransport>(
        std::move(peers), _reactor, *_connector, _inbound, _logger, _metrics, _identity, _nonces, options);

    // A signed verdict that this node's own key is revoked reaches the formation (#1555): how a
    // learner offline through its forget learns of it. Set before `Start`, as the transport asks.
    if (_hooks.onOwnKeyRevoked)
        _transport->ObserveOwnKeyRevoked(_hooks.onOwnKeyRevoked);

    auto recovered = _storage.Load();
    if (!recovered.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::ConsensusStore,
                                         std::format("cannot recover consensus state: {}", recovered.error().context)) };

    // `Create` rather than the constructor, which is private precisely so the
    // configuration validation cannot be bypassed by omission -- so there is no
    // separate `Validate()` call here to forget.
    auto raftConfig = Consensus::RaftConfig { .self = cfg.nodeId, .voters = std::move(ids), .learners = {} };
    // Read off the configuration the node is built from, so what each leader-contact reading is
    // judged by is the timing this node's consensus actually runs, never a copy of its default.
    _electionTimeoutMax = raftConfig.electionTimeoutMax;
    auto node = Consensus::RaftNode::Create(
        std::move(raftConfig), *_random, std::chrono::steady_clock::now(), *std::move(recovered));
    if (!node.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::EarlierRule, node.error().context) };

    // Read from the node rather than left at its default, because a node recovered
    // from storage comes back at whatever term it had reached. Only the term can
    // differ -- a recovered node is always a follower knowing no leader -- so this
    // changes what the first line SAYS and not what it announces.
    //
    // Read BEFORE the driver exists, and that order is #1542's: building the driver
    // restores a recovered snapshot into `_application`, whose observer publishes the
    // state -- the member set, the revoked keys, and a role announcement through
    // `Republish`. Read after, that first announcement would name term 0.
    _lastTerm = node->CurrentTerm();

    // `Create` hands a recovered snapshot to `_application` before anything can apply an
    // entry above it, so the replicated member set, settings and revoked keys are back --
    // and published -- before either loop below starts.
    //
    // Or it REFUSES, and then this node does not start (#1542): its own snapshot, or a
    // command its own log holds, is one this build cannot read, and the application was
    // handed nothing. Running on what it could read would be running without the
    // members, the settings and the revoked keys -- removal failing OPEN, loudly or
    // not. Named here, where the directory is known: the refusal says where in the state
    // and which versions, and the remedy is the store's own, since the three files go
    // aside together whichever of them could not be read.
    auto driver =
        Consensus::RaftDriver::Create(*std::move(node),
                                      _storage,
                                      *_transport,
                                      _application,
                                      Consensus::CompactionPolicy { .appliedEntriesBeforeCompaction = CompactAfterEntries });
    if (!driver.has_value())
        return std::unexpected { Refusal(NodeRefusalCause::ConsensusState,
                                         UnreadableConsensusStateRefusal(NodeStateDirectory(cfg), driver.error())) };
    _driver = *std::move(driver);
    _recoveredLastIndex = _driver->CurrentProgress().lastLogIndex;

    // Pushed, not polled. A poll interval is a window in which this node has stopped
    // leading and is still handing out other machines' capacity, and the driver knows
    // the moment it happens -- see `RaftDriver::RoleObserver`, and note it may call
    // this from either the timer thread or a peer reader.
    _driver->ObserveRole([this](Consensus::RaftDriver::RoleChange const& change) { PublishRole(change); });

    // Answered before either loop runs, so no surface ever reads it undecided: a driver
    // that has just been built has refused nothing. From here on the driver says when a
    // refusal starts and ends (#1552).
    if (_conditions != nullptr)
        _conditions->Clear(NodeCondition::UnreadableLeaderSnapshot);
    _driver->ObserveInstallRefusal([this](std::optional<Consensus::RaftDriver::InstallRefusal> const& refusal) {
        ReportInstallRefusal(refusal, _logger, _conditions);
    });

    // Announced BEFORE anything starts, and unconditionally. Until consensus says
    // otherwise this node is `Undecided`, which is what a node in a cluster that
    // has not elected anybody is -- and what the scheduler surface already believes,
    // since no scheduler leads before an election (#178), so saying it is a formality
    // that keeps the first publication from depending on that default. A node that
    // recovered a snapshot has already announced exactly this, from the restore's
    // publication above; `Republish` then finds nothing moved and says nothing.
    Republish();

    _sink = std::make_unique<DriverSink>(*_driver, _logger);
    // Bound here, before `_transport->Start()` below: the transport delivers into the driver it
    // was built before, and a message arriving unbound would be dropped.
    _inbound.Bind(*_sink);
    // The transport is the server's inbound links: a learner's two-way session, accepted here,
    // is how the transport writes to that learner, which nobody dials. One reactor for both. A mode
    // that binds no port accepts nothing, so it runs no server -- and one loop fewer stops the reactor.
    if (_listener != nullptr)
        _peerServer = std::make_unique<Consensus::RaftPeerServer>(
            *_listener, _reactor, *_sink, *_transport, _logger, _metrics, _identity, _nonces, _acceptLoops);

    // Both loops on ONE reactor, and neither through `core::async::syncRun`: that function
    // resumes a coroutine exactly once and throws when it is still suspended, so a
    // driver awaiting `SleepUntil` aborted the process the first time anybody
    // started three nodes.
    //
    // The accept loop is submitted first, so a peer that dials the instant this
    // node's timers start finds somebody listening. The reverse order leaves a
    // window in which this node campaigns and refuses the votes it provoked.
    auto serve = [](Consensus::RaftPeerServer* server, ConsensusTier* tier) -> core::async::DetachedTask {
        co_await server->Run();
        tier->NoteLoopFinished();
        co_return;
    };
    auto tick =
        [](Consensus::RaftDriver* driver, core::net::EventLoop* reactor, ConsensusTier* tier) -> core::async::DetachedTask {
        // The reactor arrives by pointer precisely because this is a coroutine: a
        // reference parameter is bound before the first suspension and then outlives
        // every frame that could have kept it alive.
        co_await driver->Run(reactor);
        tier->NoteLoopFinished();
        co_return;
    };

    // The outbound side owns a thread per peer and starts them on request rather
    // than at construction, so that a caller can wire everything up before any
    // dialling begins. Nothing called it, which is a defect with no diagnostic at
    // all: every node came up, listened, ticked its own timers and sent NOTHING,
    // so three nodes sat at `undecided` forever with no error anywhere.
    // The sink the transport delivers into must be bound by now: one left unbound drops every
    // message a two-way session carries, and the learner behind it never catches up.
    assert(_inbound.Bound() && "the transport's inbound sink must be bound before it starts");
    _transport->Start();

    // The loops this mode runs, in the order they are submitted -- and their COUNT is what the last one
    // to finish reads, so it is taken from this list rather than restated beside it. Counted before
    // either is submitted: a loop that ends at once must not stop the reactor under the other.
    auto loops = std::vector<std::function<void()>> {};
    if (_peerServer != nullptr)
        loops.emplace_back([&serve, this] { serve(_peerServer.get(), this); });
    loops.emplace_back([&tick, this] { tick(_driver.get(), &_reactor, this); });
    _loopsRunning.store(static_cast<int>(loops.size()), std::memory_order_relaxed);
    for (auto const& submit: loops)
        submit();

    _ioThread = std::jthread { [this] { _reactor.run(); } };

    // Third thread, and it is the one that can afford to be: it holds no socket and
    // does nothing at all in the ordinary case. What it may NOT do is run on either
    // of the other two -- a proposal is a durability write and a broadcast, and both
    // of those loops exist precisely to not be held up by one.
    _reconcileThread = std::jthread { [this](std::stop_token const& stop) {
        while (!stop.stop_requested())
        {
            Reconcile();

            // A stop ends the wait at once: one that had to wait out a full interval makes
            // teardown look hung, which this repository has already paid for once as a
            // `systemctl stop` that escalated to SIGKILL.
            if (WaitForStopOr(stop, ReconcileInterval) == WaitEnd::Stopped)
                break;
        }
    } };
    return {};
}

ConsensusTier::~ConsensusTier()
{
    // Order is the whole of it, and nothing here stops the reactor: the two loops
    // do that themselves when the second of them finishes, so `Run()` never returns
    // while a coroutine is still parked on it.
    //
    // `Shutdown` closes the listener AND every accepted connection, which is what
    // completes each parked read and lets its task reach its own end. The driver's
    // loop observes its stop when its current wait expires, which is bounded by the
    // heartbeat interval. The `jthread` joins in its own destructor after that, in
    // reverse declaration order, which is what the member ordering buys.
    // The transport goes FIRST, and the order is load-bearing. The reactor is
    // stopped when the second of the two COUNTED loops finishes, and peer senders
    // are not counted -- so if the tick loop ended while a sender were still
    // parked, the reactor would return with that frame suspended and nobody would
    // ever resume or free it. Draining the senders while the reactor is still
    // running means that by the time either counted loop ends there is nothing
    // else parked on it.
    //
    // Stopping it early is safe: `Send` after a stop is already a no-op, and the
    // driver keeps ticking against a transport that drops for at most one
    // heartbeat interval, which `IRaftTransport` is best-effort about anyway.
    //
    // Each phase says it is over at Debug. Two of them are bounded drains that report their
    // own ceiling at Error, and the third -- the joins below this body -- is not bounded at
    // all, so a stop that does not finish is told apart by the LAST of these lines it left: a
    // teardown hang on a CI leg, never reproduced locally, had nothing else to say which.
    if (_transport != nullptr)
        _transport->Stop();
    _logger.Log(LogLevel::Debug, "consensus: stopping: the peer senders' drain is over");
    if (_peerServer != nullptr)
        _peerServer->Shutdown();
    _logger.Log(LogLevel::Debug, "consensus: stopping: the peer server's drain is over");
    if (_driver != nullptr)
        _driver->Stop();

    // The request is the wakeup: the reconciler's wait takes part in its stop token
    // (`WaitForStopOr`), so nothing needs notifying beside it.
    _reconcileThread.request_stop();
    _logger.Log(LogLevel::Debug, "consensus: stopping: joining the reconciler, then the reactor once its loops end");
}

std::expected<Consensus::LogIndex, ConsensusError> ConsensusTier::Propose(Cluster::Command const& command)
{
    // A forget is PREPARED here first, by the one node that can (#1539, #1555): against
    // the configuration consensus holds, so forgetting the only voter is refused by name
    // while the operator who typed it is reading the answer; and with the key THIS node
    // holds live for the id, so a member its own bootstrap roster names with a key and the
    // state records nowhere has that key revoked too.
    auto proposal = command;
    if (command.kind == Cluster::CommandKind::Forget)
    {
        // Against the state this node has applied, which is what says whether anything records
        // a key for the id: a forget that would revoke nothing is refused here, by name.
        auto prepared = Cluster::PrepareForget(
            _application.State(), _driver->CurrentProgress().configuration, command.key, _roster.KeysOf(command.key).live);
        if (!prepared.has_value())
            return std::unexpected { prepared.error() };
        proposal = *std::move(prepared);
    }

    // Validated BEFORE it is proposed, which is the only place a change can be
    // refused: an entry is applied after it is committed, when there is nobody left
    // to report a failure to and no way to un-commit it.
    //
    // Against the state this node has applied (#178), because the rules about keys are
    // rules about what the roster already holds: a revoked key, one held under another id.
    if (auto const allowed = Cluster::ValidateAgainst(_application.State(), proposal); !allowed.has_value())
        return std::unexpected { allowed.error() };

    return _driver->Propose(Cluster::Encode(proposal), std::chrono::steady_clock::now());
}

Consensus::RaftWire::SessionDirection ConsensusTier::Direction() const noexcept
{
    return _transport->Direction();
}

std::vector<Consensus::NodeId> ConsensusTier::DialTargets() const
{
    return _transport->DialTargets();
}

Cluster::ClusterState ConsensusTier::ClusterState() const
{
    return _application.State();
}

std::function<ConsensusStatus()> ConsensusScrapeSource(ConsensusTier const* tier)
{
    // Null is the ordinary single-machine deployment, not a degraded one: a node
    // without `--listen-raft` runs no consensus at all, and an empty function is what
    // leaves `MetricsSnapshot::consensus` disengaged so the renderer emits no
    // consensus series whatsoever. A default-constructed `ConsensusStatus` here would
    // instead report a cluster of nobody, which is a reading rather than an absence.
    if (tier == nullptr)
        return {};
    return [tier] {
        return tier->Status();
    };
}

ConsensusStatus ConsensusTier::Status() const
{
    // ONE read of the driver, so the five facts describe one moment. Asking
    // `_driver->Node()` for them instead would be five reads with no lock at all --
    // that accessor says so in as many words -- and the moment they disagree is an
    // election, which is exactly when somebody is looking.
    return ConsensusStatusFrom(_driver->CurrentProgress());
}

std::optional<Consensus::Standing> ConsensusTier::CurrentStanding() const
{
    // From the configuration consensus HOLDS and this node's own id, through the one
    // author of the answer -- the question `--node-status` asks, and a different one from
    // the seat the replicated record names (#1449).
    return Consensus::Membership::StandingOf(_driver->CurrentProgress().configuration, _self.id);
}

AppliedStateReading ConsensusTier::CurrentAppliedState() const
{
    auto const progress = _driver->CurrentProgress();
    return AppliedStateOf(progress.appliedIndex, progress.lastLogIndex, _recoveredLastIndex);
}

std::expected<void, ConsensusError> ConsensusTier::ProposeToCluster(Cluster::Command const& command)
{
    return Propose(command).transform([](Consensus::LogIndex) {});
}

void ConsensusTier::NoteAnnouncedEndpoint(Consensus::NodeId const& member, std::string endpoint)
{
    // Kept, never proposed here: the scheduler's thread calls this, a proposal is a durability
    // write, and the reconcile pass already decides every record in one place -- including whether
    // this member's record differs at all and whether a change for it is still in flight.
    auto const guard = std::unique_lock { _desiredMutex };
    _announced.insert_or_assign(member, std::move(endpoint));
}

void ConsensusTier::Desire(std::span<Cluster::DesiredMember const> records)
{
    auto const guard = std::unique_lock { _desiredMutex };
    for (auto const& record: records)
    {
        // Replaced rather than appended when the id is already desired, so a caller
        // handing over the same peers on every pass of its own loop grows nothing --
        // and so a peer that MOVED supersedes its own older record instead of
        // sitting beside it, which would make the reconciler propose two addresses
        // for one node in a fixed order forever.
        auto const it = std::ranges::find(_desired, record.id, &Cluster::DesiredMember::id);
        if (it != _desired.end())
            *it = record;
        else
            _desired.push_back(record);
    }
}

void ConsensusTier::NoteLoopFinished() noexcept
{
    // The LAST one stops the reactor. `core::net::EventLoop::Run` returns with its timer heap
    // and its parked work exactly where they were, so stopping it while either loop
    // is still suspended would leave a coroutine frame nobody ever resumes and
    // nobody ever frees -- a leak a sanitizer reports and a long-lived process pays
    // for.
    if (_loopsRunning.fetch_sub(1, std::memory_order_acq_rel) == 1)
        _reactor.stop();
}

void ConsensusTier::Reconcile()
{
    auto const state = _application.State();

    // Every pass, before anything below can return early: how long this node's applied state has
    // gone unrefreshed by a leader it counts is what bounds the grants its worker honours.
    if (_onLeaderContact)
        _onLeaderContact(LeaderReadingOf(_driver->CurrentProgress(), _clock.now()), _electionTimeoutMax);

    // Every node, leader or not, and BEFORE anything is proposed. A member the
    // cluster agreed to admit has to be dialable by everybody -- the leader
    // replicates to it and every other member sends it votes -- and a member
    // counted towards a quorum that nobody dials is a cluster that stops forming
    // one.
    // Snapshotted once for the whole pass. Two reads would be two different values
    // -- discovery lands on its own interval -- so a leader could dial one set and
    // propose from another.
    auto desired = std::vector<Cluster::DesiredMember> {};
    auto announced = Cluster::AnnouncedEndpointMap {};
    {
        auto const guard = std::unique_lock { _desiredMutex };
        desired = _desired;
        // Dropped with the leadership it was announced to -- at the transition (`PublishRole`), and
        // here again for an announcement that raced it in while this node did not lead.
        if (!_leads.load(std::memory_order_relaxed))
            _announced.clear();
        announced = _announced;
    }

    // This node's own endpoints, read NOW: an accepted reload of `--advertise`, or a roam the
    // resolver re-derived, reaches the record at the next pass this node leads (`SelfDesire`). Its
    // own `0xFC` word is an assertion even empty -- and empty when only this machine could dial it,
    // by the rule every route into the record asks; its Raft one only while its seat is dialled.
    auto const advertisedRaft = _raftAdvertised.Current();
    if (auto const self = std::ranges::find(desired, _self.id, &Cluster::DesiredMember::id); self != desired.end())
        *self = SelfDesire(std::move(*self), _self.seat, _advertised.Current(), advertisedRaft);

    // Every node, leader or not: THIS node moved, so every session it dialled or holds runs over a
    // path the network may no longer route. Closed, so each sender redials at its next message rather
    // than writing into a dead path until a timeout notices. A moved leader needs no step of its own:
    // its followers' replies ride sessions to the old address, so CheckQuorum deposes it.
    if (auto ownRaft = OwnRaftEndpoint(_self.seat, advertisedRaft); !ownRaft.empty() && ownRaft != _lastOwnRaft)
    {
        if (!_lastOwnRaft.empty())
        {
            _logger.Logf(LogLevel::Info, "raft: this node moved to {}; reconnecting to every peer", ownRaft);
            _transport->ResetSessions();
        }
        _lastOwnRaft = std::move(ownRaft);
    }

    LearnMembers(state, desired);

    // Every node, because the question "what does THIS node count" has no other
    // answer anywhere. See `ReportQuorum`.
    ReportQuorum();

    // Only a leader may propose, and asking here rather than letting `Propose`
    // refuse is what keeps a follower from logging a `NotLeader` every interval for
    // as long as it is a follower -- which is most of a healthy cluster's life.
    if (!_leads.load(std::memory_order_relaxed))
    {
        _endpointsInFlight.clear();
        return;
    }
    // And dropped at every change of leadership since the last pass, however briefly it lasted: a
    // proposal made under an earlier leadership is not one this leadership is waiting on. The map is
    // this thread's alone, so the transition is SEEN here through its count (`PublishRole`).
    if (auto const changes = _leadershipChanges.load(std::memory_order_acquire); changes != _inFlightLeadership)
    {
        _endpointsInFlight.clear();
        _inFlightLeadership = changes;
    }

    // Every member's `0xFC` endpoint as it PROVED and announced it, folded into what this node
    // desires, so the proposal is the ordinary re-proposal of that member's record: its seat and
    // key kept. One change in flight per member, held until its index commits in the term it was
    // made in -- a proposal a new term will never commit frees the member at once.
    auto const progress = _driver->CurrentProgress();
    std::erase_if(_endpointsInFlight, [&progress](auto const& entry) {
        return !QuorumProposalPending(entry.second.at, entry.second.in, progress.commitIndex, progress.term);
    });
    auto inFlight = Cluster::MembersInFlight {};
    for (auto const& [id, proposal]: _endpointsInFlight)
        inFlight.insert(id);
    auto const announcements = Cluster::AnnouncedEndpointDesires(state, announced, inFlight);
    desired = Cluster::WithAnnouncedEndpoints(std::move(desired), announcements);
    // And KEPT where a member's announcement moved its Raft endpoint by the host-coupling rule: once
    // the record commits the announcement proposes nothing, and discovery's desire -- the Raft
    // endpoint a beacon stated before the move -- would otherwise re-propose the old address.
    if (std::ranges::any_of(announcements, Cluster::SpeaksForRaftEndpoint))
    {
        auto const guard = std::unique_lock { _desiredMutex };
        _desired = Cluster::WithAnnouncedRaftEndpoints(std::move(_desired), announcements);
    }

    // Outside the lock, both the decision and the proposals: a proposal is a
    // durability write and a broadcast, and holding a lock across one would stall
    // whoever is discovering peers behind whoever is writing to a disk.
    //
    // With the configuration consensus holds, because a member the state does not
    // record may still be counted there -- every bootstrap member is -- and
    // recording one as the newcomer it is not would demote it (#1535).
    //
    // A desire discovery handed over states no key, and a member is never admitted without one,
    // so the key this node holds live for the id -- a bootstrap member's, as the formation record
    // names it -- is filled
    // in for this pass only (`Cluster::WithLiveKeys`).
    auto const keyed = Cluster::WithLiveKeys(state, desired, _roster);
    auto const plan = Cluster::MembershipProposals(state, progress.configuration, keyed);
    ReportForgottenDesires(state, plan.forgotten);

    for (auto const& command: plan.proposals)
    {
        auto const proposed = Propose(command);
        if (!proposed.has_value())
        {
            // What the refusal is ABOUT decides whether the rest of the list is
            // still worth trying, and getting that wrong is silent both ways.
            //
            // A refusal about the MOMENT -- and the commonest by far is that
            // leadership moved between the check above and here -- says nothing
            // about this command and everything about the next one, so the pass
            // ends. That is not a fault and is not repaired by re-offering the
            // rest against the same lost term.
            //
            // A refusal about the COMMAND is permanent: offering it again next
            // interval changes nothing. Returning on one is what turns a single
            // bad record into a cluster that stops admitting ANYBODY, because the
            // rest of the proposals and `ReconcileQuorum` below are skipped every
            // pass forever, with one line per interval as the only symptom. It is
            // half of the trap #159 records, and nothing in this build can produce
            // one any more -- discovery will not remember a peer it cannot name
            // (`PeerDirectory::NoteBeacon`), and the option table refuses a
            // `--node-id` or `--cluster-admit` that is not text before this process
            // starts (`ParseUtf8Text`, #155). Which is exactly why it is worth
            // being loud rather than fatal.
            if (SubjectOf(proposed.error().code) == RefusalSubject::Moment)
            {
                _logger.Logf(
                    LogLevel::Info, "cluster: cannot record {} right now: {}", command.key, proposed.error().context);
                return;
            }

            // Already true, so there is nothing to report and nothing to skip past
            // -- Debug rather than silence, because "the pass did nothing" and "the
            // pass was not reached" are the two states a quiet log cannot tell
            // apart. No `Cluster::Command` produces this today; the arm is here
            // because `SubjectOf` is a total function over the code and a `Satisfied`
            // falling through to the Warn below would announce a healthy no-op as a
            // record somebody must go and correct.
            if (SubjectOf(proposed.error().code) == RefusalSubject::Satisfied)
            {
                _logger.Logf(LogLevel::Debug, "cluster: {} is already recorded: {}", command.key, proposed.error().context);
                continue;
            }

            // Warn, and "never" rather than "right now", because the two want
            // different things from whoever reads them: one is a leader election
            // in progress and the other is a record that has to be corrected
            // before it can ever be agreed.
            _logger.Logf(
                LogLevel::Warn, "cluster: {} can never be recorded as it stands: {}", command.key, proposed.error().context);
            continue;
        }

        if (std::ranges::contains(announcements, command.key, &Cluster::DesiredMember::id))
            _endpointsInFlight.insert_or_assign(command.key, EndpointProposal { .at = *proposed, .in = progress.term });

        _logger.Logf(LogLevel::Info,
                     "cluster: recorded {} {}{}",
                     command.key,
                     DescribeConsensusEndpoint(command.value),
                     command.schedulerEndpoint.empty() ? std::string {}
                                                       : std::format(", scheduler {}", command.schedulerEndpoint));
    }

    // Against the state read at the top, which is deliberately the APPLIED one: a
    // member reaches it only after its record has committed, so the address is
    // agreed before the id is counted. That ordering is what lets every other node
    // learn where the new member answers before it is asked to vote for anybody.
    ReconcileQuorum(state);
}

void ConsensusTier::ReportForgottenDesires(Cluster::ClusterState const& state,
                                           std::span<Cluster::DesiredMember const> refused)
{
    // Once per member rather than once per pass (#1528). A desire for a forgotten member
    // comes back at every pass for as long as whatever holds it runs, so it is refused on
    // every pass -- a line per interval is a line an operator filters out, and
    // the one fact worth saying is that the forget is being honoured while something
    // still asks for the member back.
    for (auto const& member: refused)
    {
        if (std::ranges::find(_reportedForgotten, member.id) != _reportedForgotten.end())
            continue;
        _reportedForgotten.push_back(member.id);

        // The key the forget revoked, which is what the machine is forgotten BY: an address is
        // not an identity, and the desire's endpoint says only where it asked from this time.
        auto revoked = std::string {};
        for (auto const& entry: state.revokedKeys)
            if (entry.id == member.id)
                revoked += std::format("{}{}", revoked.empty() ? "" : ", ", FormatEd25519PublicKey(entry.publicKey));
        _logger.Logf(LogLevel::Info,
                     "cluster: not recording {} {}: the cluster forgot it and revoked its key ({}), and only "
                     "--cluster-admit under a new key undoes a forget",
                     member.id,
                     DescribeConsensusEndpoint(member.raftEndpoint),
                     revoked);
    }

    // A member no longer refused is forgotten here too, so forgetting it a SECOND time --
    // after an operator re-admitted it -- is said again rather than swallowed.
    std::erase_if(_reportedForgotten, [refused](Consensus::NodeId const& id) {
        return std::ranges::find(refused, id, &Cluster::DesiredMember::id) == refused.end();
    });
}

void ConsensusTier::LearnMembers(Cluster::ClusterState const& state, std::span<Cluster::DesiredMember const> desired)
{
    auto const learn = [this](Consensus::NodeId const& id, std::string const& endpoint) {
        auto const where = PeerEndpointFor(id, endpoint);
        if (!where.has_value())
            return;

        auto const change = _transport->Learn(*where);

        // Only the two that changed something. `Unchanged` is what a healthy fleet
        // answers on every pass forever, and `Self` is this node's own record, which
        // a member set always contains -- reporting either would bury the two lines
        // that say the cluster's shape moved.
        if (change == Consensus::PeerChange::Added)
            _logger.Logf(LogLevel::Info, "raft: now dialling peer {} at {}", id, endpoint);
        else if (change == Consensus::PeerChange::Readdressed)
            _logger.Logf(LogLevel::Info, "raft: peer {} moved to {}", id, endpoint);
    };

    // Only a member whose seat is DIALLED (`Cluster::LinkOfSeat`): a learner dials in, and its
    // recorded endpoint -- empty, or one it once had -- is nobody's to dial. So a leader never dials
    // a learner, and a learner dials the voters and no other learner.
    for (auto const& member: state.members)
        if (Cluster::LinkOfSeat(member.seat) == Consensus::PeerLink::Dialled)
            learn(member.id, member.raftEndpoint);

    // Which members reach this node by dialling in is the link column's, read from the record's
    // seats AND the configuration's standings (`DialInPeers`) and never decided here -- so the
    // transport counts a message for a learner with no session attached as exactly that, and one
    // for a peer it cannot place as the other. The whole set on every pass, since a promotion or
    // a forget takes one out.
    _transport->LearnDialsIn(DialInPeers(state, _driver->CurrentProgress().configuration));

    // And what discovery has proved, which the state may not hold yet -- or ever,
    // on a node that is not the leader and so proposes nothing. A peer that has
    // answered the key challenge is one this node has every reason to dial: for a
    // node waiting to be admitted it is the ONLY route to an address, and without
    // it such a node cannot answer the leader that admits it.
    //
    // Only where the state is silent, and the precedence is the point rather than
    // the saving. The two can disagree about one peer's address -- a node that
    // moved, seen by discovery before the change is agreed -- and learning both
    // would re-address it twice and drop its connection twice, once per pass,
    // forever. What the cluster has AGREED wins over what one node believes.
    for (auto const& member: desired)
        if (std::ranges::find(state.members, member.id, &Cluster::ClusterMember::id) == state.members.end())
            learn(member.id, member.raftEndpoint);
}

void ConsensusTier::ReportQuorum()
{
    // The only place a node says what its own consensus configuration is.
    //
    // Nothing else could: `--cluster-status` reports the FLEET's member record from
    // `ClusterStateMachine`, which is a different set, and only a leader answers it
    // at all -- so the one node whose view you need during a stall is the one that
    // redirects you elsewhere. There is no consensus counter either. That gap is
    // why #388 read as an unexplainable silence: a joiner that never adopted a
    // configuration is excused from every deadline, campaigns in no election, and
    // logs NOTHING while doing it, which is indistinguishable from a healthy
    // follower with nothing to say.
    //
    // #435 is the surface this should eventually be; a line an operator can already
    // read is what can be had without one.
    auto configuration = _driver->CurrentProgress().configuration;
    std::ranges::sort(configuration.voters);
    std::ranges::sort(configuration.learners);

    // The roster learns what this node counts every pass, reported or not (#1555): a member a
    // forget revoked keeps its key on this wire until the configuration drops it, or a cluster
    // losing its leader inside that pass could be left with a quorum nobody can reach.
    _roster.AdoptConfiguration(configuration);
    // And every session a peer dialled in on is asked again NOW rather than at its next frame: a
    // forgotten learner's session carries none, so without this it idles on and the learner never
    // hears it was forgotten (`RaftPeerTransport::RecheckProofs`).
    _transport->RecheckProofs();

    // The FIRST pass reports whatever it finds, changed or not, and that is the
    // point rather than an initialisation detail. A joiner starts with no members,
    // so a report that only fired on a CHANGE would say nothing at all about the
    // state that matters -- and silence would then mean both "this node counts no
    // cluster" and "this loop never ran". Those are different failures.
    if (_quorumReported && configuration == _reportedConfiguration)
        return;

    _quorumReported = true;
    _reportedConfiguration = std::move(configuration);

    if (Consensus::Membership::IsEmpty(_reportedConfiguration))
    {
        // Said out loud, because it is a legitimate state for a node that joined a fleet
        // and a fatal one for any other -- and the two are told apart by which node
        // logged it, not by the line.
        _logger.Log(LogLevel::Info, "consensus: this node counts no cluster of its own; it is waiting to be admitted");
        return;
    }

    auto const join = [](std::vector<Consensus::NodeId> const& ids) {
        auto names = std::string {};
        for (auto const& id: ids)
            names += (names.empty() ? "" : ", ") + id;
        return names;
    };

    // The COUNT is the voters, since a quorum counts nobody else, and the line keeps the
    // prefix it has always had so a watcher of it keeps working. Then the learners, only
    // when there are any, and last this node's own standing (#1449): the half an operator
    // cannot read off the lists without knowing which id this node is, and the one that
    // says whether it will ever stand -- a learner counting itself among nobody is not
    // the #388 fault, and a watcher that must tell the two apart reads this word.
    auto const standing = Consensus::TraitsOf(Consensus::Membership::StandingOf(_reportedConfiguration, _self.id)).name;
    auto const learners = _reportedConfiguration.learners.empty()
                              ? std::string {}
                              : std::format("; learners, counted by no quorum: {}", join(_reportedConfiguration.learners));
    _logger.Logf(LogLevel::Info,
                 "consensus: this node counts {} member(s): {}{}; it is a {}",
                 _reportedConfiguration.voters.size(),
                 join(_reportedConfiguration.voters),
                 learners,
                 standing);
}

void ConsensusTier::ReconcileQuorum(Cluster::ClusterState const& state)
{
    // Both under one lock, because they are compared: two reads would let a
    // configuration change land between them and produce a pair that never existed.
    auto const progress = _driver->CurrentProgress();

    // One change at a time, and only once the last has been agreed. `RaftNode`
    // refuses a second while one is in flight, so proposing anyway would cost a
    // refusal per interval -- and the wait itself is the diagnostic that matters,
    // because a configuration naming a member that will never acknowledge this
    // leader never commits and is otherwise completely silent.
    if (QuorumProposalPending(_quorumProposedAt, _quorumProposedIn, progress.commitIndex, progress.term))
    {
        ++_quorumWaited;

        // Once, at a round number of passes, rather than every pass: a wait that
        // logs per interval is a wait an operator filters out.
        if (_quorumWaited == QuorumProposalPatience)
            _logger.Logf(LogLevel::Warn,
                         "cluster: the membership change at index {} has not committed after {} seconds; a member it "
                         "names may not accept this node as its leader",
                         _quorumProposedAt.value,
                         (QuorumProposalPatience * ReconcileInterval).count() / 1000);
        return;
    }

    _quorumWaited = 0;

    // With what this leader knows of every member's log, from the same read as the
    // configuration and the commit index: a promotion waits until the member has caught
    // up (#1537), and the members it is waiting for are named.
    auto const plan = Cluster::NextQuorumChange(
        state,
        progress.configuration,
        _self,
        _bootstrapIds,
        Cluster::Replication { .commitIndex = progress.commitIndex, .matchIndex = progress.matchIndex });
    ReportCatchingUp(plan.catchingUp, progress);

    auto const& change = plan.change;
    if (!change.has_value())
        return;

    auto const proposed = _driver->ProposeMembership(*change, std::chrono::steady_clock::now());
    if (!proposed.has_value())
    {
        // Classified by CODE, which #196 is what made possible: this path used to
        // treat every refusal as the moment it usually is, because
        // `ProposeMembership` answered `InvalidConfiguration` for two conditions
        // that are not permanent and `SubjectOf` would have called them so. Wiring
        // it in was the obvious next tidy-up and would have reported "wait for the
        // change in flight to commit" as **can never be recorded as it stands**, at
        // Warn, every interval, for a condition that resolves itself in one commit.
        //
        // The commonest reason really is that leadership moved between the two,
        // which is not a fault -- and that is now a `Moment` because the code says
        // so, rather than because this comment assumed it.
        switch (SubjectOf(proposed.error().code))
        {
            case RefusalSubject::Moment:
                _logger.Logf(LogLevel::Info, "cluster: cannot change the quorum right now: {}", proposed.error().context);
                break;
            case RefusalSubject::Satisfied:
                // `NextQuorumChange` does not propose an unchanged set, so reaching
                // this is a disagreement between it and the configuration this node
                // just read -- worth a line, worth nobody being paged.
                _logger.Logf(LogLevel::Debug, "cluster: the quorum already reads as proposed: {}", proposed.error().context);
                break;
            case RefusalSubject::Command:
                // Permanent, and therefore the one an operator has to act on: a
                // member set that no validator will ever accept is not repaired by
                // another interval.
                _logger.Logf(
                    LogLevel::Warn, "cluster: the quorum can never be changed as proposed: {}", proposed.error().context);
                break;
        }
        return;
    }

    _quorumProposedAt = *proposed;

    // Recorded together, because the pair is what the wait above is asked about: an
    // index without its term cannot say whether the proposal it names can still be
    // the one that lands.
    _quorumProposedIn = progress.term;
    _logger.Logf(LogLevel::Info,
                 "cluster: proposing a quorum of {} voter(s) and {} learner(s) at index {}",
                 change->voters.size(),
                 change->learners.size(),
                 _quorumProposedAt.value);

    // Said on its own line, because it is the one change after which this node stops
    // leading (#1539), and an operator watching a leader go quiet should not have to
    // infer why from a count.
    if (!Consensus::Membership::IsMember(*change, _self.id))
        _logger.Logf(LogLevel::Info,
                     "cluster: the cluster forgot this node ({}), so it proposes its own removal and steps down once "
                     "that commits",
                     _self.id);
}

void ConsensusTier::ReportCatchingUp(std::span<Consensus::NodeId const> waiting,
                                     Consensus::RaftDriver::Progress const& progress)
{
    // Said when the wait starts, and again -- at Warn -- once it has lasted as long as an
    // uncommitted change is allowed to before it is reported: a voter an operator
    // admitted or promoted, held as a learner, is otherwise visible only as a record and
    // a standing that disagree. Once each rather than per pass, for the reason every
    // report in this loop is.
    for (auto const& id: waiting)
    {
        auto const [entry, started] = _catchingUp.try_emplace(id, 0);
        ++entry->second;

        auto const match = progress.matchIndex.find(id);
        auto const held = match != progress.matchIndex.end() ? match->second.value : 0;
        if (started)
            _logger.Logf(LogLevel::Info,
                         "cluster: {} is recorded as a voter and counted as a learner until it has caught up: it holds "
                         "entry {} of the {} committed",
                         id,
                         held,
                         progress.commitIndex.value);
        else if (entry->second == QuorumProposalPatience)
            _logger.Logf(LogLevel::Warn,
                         "cluster: {} has not caught up after {} seconds (entry {} of {}), so no quorum counts it yet; "
                         "it is promoted once it holds every committed entry",
                         id,
                         (QuorumProposalPatience * ReconcileInterval).count() / 1000,
                         held,
                         progress.commitIndex.value);
    }

    // A member no longer waiting is forgotten here, so a later wait is said again.
    std::erase_if(_catchingUp,
                  [waiting](auto const& entry) { return std::ranges::find(waiting, entry.first) == waiting.end(); });
}

void ConsensusTier::PublishRole(Consensus::RaftDriver::RoleChange const& change)
{
    // The cause first, and unconditionally: a higher term arriving is an event
    // rather than a state, so the suppression `Republish` applies -- which exists
    // to stop an unchanged *announcement* being repeated -- would be the wrong
    // question to ask about it.
    if (change.cause.has_value())
        _logger.Log(LogLevel::Info, DescribeTermAdoption(change.term, *change.cause));

    _lastRole = change.role;
    _lastTerm = change.term;
    auto const leaderMoved = _lastLeader != change.knownLeader;
    _lastLeader = change.knownLeader;
    Republish();

    // Who leads is part of what the formation reads off the applied state, and it can move with no
    // state applied at all.
    if (leaderMoved)
        TellFormation(_application.State());
}

void ConsensusTier::OnStateChanged(Cluster::ClusterState const& state)
{
    // The roster FIRST, so the keys every peer connection is judged by are the committed
    // ones before anything else reacts to the change. An applied `Forget` reaches every
    // open session its revoked key proved from here: each re-asks the roster before its next
    // frame (#178, #1555).
    _roster.Adopt(state);

    // The keys reach the fleet's oracle from here, so admitting a machine and serving it are
    // one decision rather than two facts that can disagree.
    if (_onMembers)
        _onMembers(state);

    // And the leader's ADDRESS may have just arrived, which is a different answer
    // from the role this node already knew.
    Republish();

    TellFormation(state);
}

void ConsensusTier::TellFormation(Cluster::ClusterState const& state)
{
    if (!_hooks.onState)
        return;
    // Where the leader answers the `0xFC` port is the state's to say; this node's own too, since a
    // formation reads "who leads" the same way whoever leads.
    auto const leaderNodeEndpoint =
        _lastLeader.has_value() ? state.SchedulerEndpointOf(*_lastLeader).value_or(std::string {}) : std::string {};
    _hooks.onState(state, _clusterId, _lastLeader, leaderNodeEndpoint);
}

void ConsensusTier::Republish()
{
    auto const scheduled = SchedulerRoleFor(_lastRole, _lastLeader);

    // The endpoint rather than the id, because a client redirects to an ADDRESS. The
    // replicated state is what knows the mapping, which is the second reason a member
    // carries its scheduler endpoint: without it a follower could name its leader and
    // not say where it is, which is a redirect nobody can follow.
    auto leaderEndpoint = std::string {};
    if (scheduled != Distributed::SchedulerRole::Leader && _lastLeader.has_value())
        leaderEndpoint = _application.State().SchedulerEndpointOf(*_lastLeader).value_or(std::string {});

    // Nothing moved, so nothing is announced. Without this the state observer would
    // log a role line on every committed entry -- and an observer told the same
    // thing repeatedly is one whose callers cannot use "I was told" to mean
    // anything.
    //
    // Two tests rather than one, because the log and the scheduler are asking
    // different questions. A node campaigning round after round without winning is
    // `Undecided` with no endpoint every time, so a single test leaves the one
    // condition somebody reads a dump to find completely silent.
    //
    // A moved TERM is now a real change for the scheduler too, which it was not
    // before #322: the term goes inside every grant it mints, so a node that stayed
    // `Leader` across an election and was told nothing would keep stamping the
    // previous one -- and the whole point of covering the epoch is that a grant names
    // the term it was actually issued under. Re-announcing an UNCHANGED role with an
    // unchanged term is still forbidden, which is what the paragraph above is about.
    auto const announcementMoved =
        !_published || scheduled != _publishedRole || leaderEndpoint != _publishedEndpoint || _lastTerm != _publishedTerm;
    if (!announcementMoved && _lastTerm == _publishedTerm)
        return;

    auto const termMoved = _lastTerm != _publishedTerm;
    _published = true;
    _publishedTerm = _lastTerm;

    // The line is written when EITHER moved, which is what keeps a node campaigning
    // without winning visible; the observer is called when the announcement moved,
    // and a moved term is now part of that.
    if (announcementMoved || termMoved)
        _logger.Log(LogLevel::Info, DescribeRole(scheduled, _lastTerm, leaderEndpoint));

    if (!announcementMoved)
        return;

    _publishedRole = scheduled;
    _publishedEndpoint = leaderEndpoint;

    // The announced endpoints go WITH the leadership they were announced to, at the transition
    // rather than at the next pass that samples it: a leader that lost and regained leadership
    // between two passes would otherwise re-propose a map older than whatever the leader in between
    // recorded. Either direction clears -- one gained is a map nobody announced to this node -- and
    // the reconciler's in-flight endpoints go at its next pass, through the change count.
    //
    // Keyed on the TERM as well as the flag: a leadership lost and regained that this observer sees
    // only as Leader(t5) then Leader(t7) never flips the flag, and a leader's term never moves while
    // it leads -- so a moved term while leading is a new leadership too.
    auto const leads = scheduled == Distributed::SchedulerRole::Leader;
    if (auto const flipped = _leads.exchange(leads, std::memory_order_relaxed) != leads; flipped || (leads && termMoved))
    {
        auto const guard = std::unique_lock { _desiredMutex };
        _announced.clear();
        _leadershipChanges.fetch_add(1, std::memory_order_release);
    }

    if (_onRole)
        // .value, because Term is a distinct type here and a plain integer on the
        // wire: the token carries a number, and the type exists to stop terms being
        // confused with log indices inside consensus rather than outside it.
        _onRole(scheduled, leaderEndpoint, _lastTerm.value);
}

ConsensusTier::LeaderContactObserver LeaderContactObserverFor(NodeRoster& roster,
                                                              SchedulingLeaderPublisher& schedulingLeader)
{
    return [&roster, &schedulingLeader](Distributed::LeaderReading const& reading,
                                        std::chrono::milliseconds electionTimeoutMax) {
        roster.ConsensusPass(reading);
        schedulingLeader.LeaderContact(reading, electionTimeoutMax);
    };
}

std::expected<std::unique_ptr<ConsensusTier>, NodeRefusal> StartConsensusOrExplain(
    NodeConfig const& cfg,
    std::unique_ptr<SchedulerTier> const& schedulerTier,
    Cc::IAdvertisedEndpointSource const& advertised,
    Cc::IAdvertisedEndpointSource const& raftAdvertised,
    std::optional<Ed25519KeyPair> const& identityKey,
    NodeMembership& membership,
    NodeRoster& roster,
    AppliedSchedulers& schedulers,
    SchedulingLeaderPublisher& schedulingLeader,
    SharedCacheListeners sharedCache,
    IMetricsSink& metrics,
    ILogger& logger,
    NodeConditions* conditions,
    FormationHooks hooks)
{
    // No cluster configured: a pure worker, which since #178 is the only node that runs
    // none -- a scheduler is a consensus member even alone.
    //
    // Asked through `RunsConsensus` rather than spelled here, because the startup table
    // asks the same question of a scheduler, and while two tiers spelled it for
    // themselves they were two authors of one rule (#613).
    if (!RunsConsensus(cfg))
        return std::unique_ptr<ConsensusTier> {};

    auto tier = ConsensusTier::Start(
        cfg,
        advertised,
        raftAdvertised,
        identityKey,
        [&schedulerTier,
         &schedulingLeader](Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t term) {
            // Told on every node, scheduler or not: a node running none still names the leader
            // it follows to the launchers that ask it to schedule (#1639). The endpoint is the
            // member record's `0xFC` one (`Republish`), never its Raft one.
            schedulingLeader.LeaderChanged(leaderEndpoint);

            // Null when this node runs no scheduler surface, which is a legitimate
            // shape: a LEARNER, which applies the fleet's state and may contribute CPU
            // without handing out anybody's work. It neither votes nor leads, so it has
            // no role of its own to apply; it only names the leader, above.
            if (schedulerTier != nullptr)
                schedulerTier->SetRole(role, leaderEndpoint, term);
        },
        [&membership, &roster, &schedulers, sharedCache](Cluster::ClusterState const& state) {
            // The member set no longer joins admission; its KEYS do. A machine the cluster
            // agreed to admit is served by every surface at once by the key it proves or
            // presents, never by the address it dials from. `--fleet-open` is this node's own
            // answer and survives every commit (#251).
            //
            // The whole STATE, because the `fleet-open` row is an admission decision
            // too and had no reader at all until #1112. `NodeMembership` resolves it
            // against this node's own flag rather than the resolution happening here:
            // the widening guard needs to be inside the oracle, which is the object
            // every surface bound at construction (#405).
            membership.PublishCluster(state);

            // And the roster every grant this node's worker checks is verified against (#178):
            // the committed voters and revocations, the moment they are committed.
            roster.Applied(state);

            // And where this node registers: the voters' `0xFC` endpoints as the state records
            // them, so a voter that moved, proven, is reached at the next round (T26's carry).
            schedulers.Applied(state);

            // And the fleet's shared cache: where it is, and whether this machine is it -- recorded
            // and handed to the host's own thread, never opened here: opening a store may walk it,
            // and this is the apply callback a voter's heartbeats wait behind. Then the private
            // tier's upstream, which re-judges what it reports from what the directory now says.
            sharedCache.Applied(state);
        },
        // And how long that state has gone without a leader it counts speaking: past
        // `LeaderSilenceBound`, every grant is refused (`consensus-leader-silent`) -- and past the
        // tier's own election timeout the leader is named to nobody (`SchedulingEndpointToPublish`).
        LeaderContactObserverFor(roster, schedulingLeader),
        metrics,
        logger,
        conditions,
        std::move(hooks));

    // Wired here rather than at construction, and the order is forced: consensus
    // needs the port the scheduler surface BOUND in order to announce where
    // clients reach this node, so the scheduler cannot be handed a cluster that
    // does not exist yet. Without this the three cluster verbs answer `NoCluster`
    // -- which is correct for a node that runs none, and would be a silent
    // no-op for one that does.
    if (tier.has_value() && *tier != nullptr && schedulerTier != nullptr)
        schedulerTier->Administer(**tier);

    return tier;
}

} // namespace FastCache::Node
