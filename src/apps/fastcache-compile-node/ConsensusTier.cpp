// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"
#include "NodeSurfaces.hpp"

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Net/PlatformConnector.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <mutex>
#include <utility>

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
            // clock: `PlatformReactor` reports steady time, so a message stamped here
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

std::string AdvertisedSchedulerEndpoint(std::string_view raftEndpoint, std::string_view schedulerBound)
{
    // Nothing to advertise when this node serves no scheduler surface, which is a
    // legitimate shape: a member that contributes CPU and consensus without handing
    // out anybody's work. Recording an endpoint for it would redirect clients at a
    // port nothing is listening on.
    if (schedulerBound.empty() || raftEndpoint.empty())
        return {};

    auto const scheduler = SplitHostPort(schedulerBound);
    auto const consensus = SplitHostPort(raftEndpoint);
    if (!scheduler.has_value() || !consensus.has_value())
        return {};

    // A v6 host arrives from `SplitHostPort` without its brackets, and every
    // consumer of this string splits it again -- so it has to go back the way it
    // came or the next split takes the wrong colon. That is the defect
    // `Core/HostPort` exists to hold in one place, and this is one of the places.
    return FormatHostPort(consensus->first, scheduler->second);
}

ConsensusTier::ConsensusTier(Cluster::ClusterMember self,
                             Consensus::FileRaftStorage storage,
                             std::string boundEndpoint,
                             RoleObserver onRole,
                             MembersObserver onMembers,
                             ILogger& logger):
    _logger { logger },
    _storage { std::move(storage) },
    _connector { std::make_unique<PlatformConnector>(_reactor, _resolver, _clock) },
    _application { logger, [this](Cluster::ClusterState const& state) { OnStateChanged(state); } },
    _onRole { std::move(onRole) },
    _boundEndpoint { std::move(boundEndpoint) },
    _self { std::move(self) },
    _onMembers { std::move(onMembers) }
{
    // Seeded with this node's own record, and its scheduler endpoint travels as a
    // value that is PRESENT even when it is empty. That is an assertion -- "I know
    // what mine is" -- where a peer discovered on the segment has no opinion at
    // all, and `DesiredMember` keeps the two apart precisely so one cannot clear
    // what the other announced.
    _desired.push_back(Cluster::DesiredMember {
        .id = _self.id, .raftEndpoint = _self.raftEndpoint, .schedulerEndpoint = _self.schedulerEndpoint });
}

std::expected<std::unique_ptr<ConsensusTier>, std::string> ConsensusTier::Start(
    NodeConfig const& cfg, std::string_view schedulerBound, RoleObserver onRole, MembersObserver onMembers, ILogger& logger)
{
    // The bootstrap set, and this node must be in it. A node whose own id names no
    // member could never win a vote and could never be voted for -- it would stand
    // for election forever against a cluster that has never heard of it, which from
    // the outside is a node that simply never becomes ready.
    //
    // Neither half is decided here any more. The grammar belongs to the option
    // table, so `cfg.raftPeers` holds members or the command line was refused where
    // it was typed; and the rule below is `StartupPolicyRejection`'s, asked through
    // its predicate and answered in its words. That table is what makes an operator
    // hear it while they are watching -- an install consults it too -- and this is
    // the same answer arriving for a `NodeConfig` nobody parsed from an argv (#168).
    auto const& members = cfg.raftPeers;

    auto const* const self = ClusterSelfMember(cfg);
    if (self == nullptr)
        return std::unexpected { std::string { NodeIdNamesNoPeerRefusal } };

    // `--raft-join` takes the SAME tokens and means something else by them: these
    // are the nodes this one can REACH, not the cluster it is a member of. So the
    // bootstrap set is empty and the node waits to be admitted -- which is the only
    // shape a cluster can admit, because a node that bootstrapped itself has
    // elected itself and afterwards refuses every leader its own configuration does
    // not name.
    //
    // It still dials all of them, and that is not an optimization: the leader
    // admitting a joiner starts replicating at its own last index, the joiner's log
    // is empty, and the leader only walks back to the beginning when the joiner
    // REFUSES. A joiner that could not send that refusal is admitted, dialled, and
    // permanently silent -- which is what the end-to-end case found the first time
    // this was tried with a one-entry list.
    auto const bootstrap = cfg.raftJoin ? std::vector<Cluster::ClusterMember> {} : members;

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
    auto const resolved = SoleEndpointOf(NodeSurface::Raft, cfg);
    if (!resolved.has_value())
        return std::unexpected { resolved.error() };
    auto const& endpoint = *resolved;

    // The listener is bound in `Launch` rather than here, because it binds against
    // the reactor and the reactor is a member of the object this has not built yet.
    // The refusal still reaches the operator: `Launch`'s error is this function's.

    auto const stateDirectory =
        cfg.clusterDir.empty() ? std::filesystem::path { "fastcache-cluster" } / cfg.nodeId : cfg.clusterDir;
    auto storage = Consensus::FileRaftStorage::Open(stateDirectory);
    if (!storage.has_value())
        return std::unexpected { std::format("cannot open {}: {}", stateDirectory.string(), storage.error().context) };

    // The record this node announces about itself, and the only place both of its
    // addresses are known at once: the consensus one is what an operator typed and
    // every peer dials, and the scheduler one is a port nobody connects to and so
    // nobody could otherwise learn.
    auto announced = *self;
    announced.schedulerEndpoint = AdvertisedSchedulerEndpoint(self->raftEndpoint, schedulerBound);

    auto tier = std::unique_ptr<ConsensusTier> { new ConsensusTier { std::move(announced),
                                                                     *std::move(storage),
                                                                     std::format("{}:{}", endpoint.host, endpoint.port),
                                                                     std::move(onRole),
                                                                     std::move(onMembers),
                                                                     logger } };

    if (auto started = tier->Launch(cfg, members, bootstrap, endpoint.host, endpoint.port); !started.has_value())
        return std::unexpected { started.error() };

    logger.Logf(LogLevel::Info,
                "consensus on {} as {} ({}, state in {})",
                tier->BoundEndpoint(),
                cfg.nodeId,
                bootstrap.empty() ? std::string { "no cluster yet; waiting to be admitted" }
                                  : std::format("{} member(s)", bootstrap.size()),
                stateDirectory.string());
    return tier;
}

std::expected<void, std::string> ConsensusTier::Launch(NodeConfig const& cfg,
                                                       std::vector<Cluster::ClusterMember> const& dialable,
                                                       std::vector<Cluster::ClusterMember> const& bootstrap,
                                                       std::string_view bindAddress,
                                                       std::uint16_t bindPort)
{
    // Bound against THIS node's reactor, which is what makes `co_await Accept()`
    // and every read inside `RaftPeerServer` actually suspend. A blocking listener
    // would serve the first peer that connects and never accept another.
    _listener = PlatformListener::Bind(_reactor, bindAddress, bindPort);

    // `IsBound()`, not a null check: `Bind` hands back a listener carrying the
    // diagnostic rather than nothing at all, so testing for null tests nothing --
    // the defect the worker's own listener records having shipped once.
    if (_listener == nullptr || !_listener->IsBound())
        return std::unexpected { std::format("cannot bind {}:{}: {}",
                                             bindAddress,
                                             bindPort,
                                             _listener ? _listener->BindError() : std::string_view { "null listener" }) };

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
            return std::unexpected { std::format("{} is not a dialable endpoint for {}", member.raftEndpoint, member.id) };

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
    auto ids = _bootstrapIds;

    _transport =
        std::make_unique<Consensus::RaftPeerTransport>(cfg.nodeId, std::move(peers), _reactor, *_connector, _logger);

    auto recovered = _storage.Load();
    if (!recovered.has_value())
        return std::unexpected { std::format("cannot recover consensus state: {}", recovered.error().context) };

    // `SystemRandomSource` unseeded, so two nodes started together do not draw the
    // same election timeout and split the vote round after round. Owned here because
    // the node holds it for its whole life; the seeded constructor exists so a
    // failure can be replayed, and nothing replays a production node.
    _random = std::make_unique<SystemRandomSource>();

    // `Create` rather than the constructor, which is private precisely so the
    // configuration validation cannot be bypassed by omission -- so there is no
    // separate `Validate()` call here to forget.
    auto node = Consensus::RaftNode::Create(Consensus::RaftConfig { .self = cfg.nodeId, .members = std::move(ids) },
                                            *_random,
                                            std::chrono::steady_clock::now(),
                                            *std::move(recovered));
    if (!node.has_value())
        return std::unexpected { node.error().context };

    _driver = std::make_unique<Consensus::RaftDriver>(
        *std::move(node),
        _storage,
        *_transport,
        _application,
        Consensus::CompactionPolicy { .appliedEntriesBeforeCompaction = CompactAfterEntries });

    // Pushed, not polled. A poll interval is a window in which this node has stopped
    // leading and is still handing out other machines' capacity, and the driver knows
    // the moment it happens -- see `RaftDriver::RoleObserver`, and note it may call
    // this from either the timer thread or a peer reader.
    _driver->ObserveRole([this](Consensus::RaftDriver::RoleChange const& change) { PublishRole(change); });

    // Read from the node rather than left at its default, because a node recovered
    // from storage comes back at whatever term it had reached. Only the term can
    // differ -- a recovered node is always a follower knowing no leader -- so this
    // changes what the first line SAYS and not what it announces.
    _lastTerm = _driver->Node().CurrentTerm();

    // Announced BEFORE anything starts, and unconditionally. Until consensus says
    // otherwise this node is `Undecided`, which is what a node in a cluster that
    // has not elected anybody is -- and the scheduler surface would otherwise go
    // on believing the standalone leadership it was constructed with.
    Republish();

    _sink = std::make_unique<DriverSink>(*_driver, _logger);
    _peerServer = std::make_unique<Consensus::RaftPeerServer>(*_listener, *_sink, _logger);

    // Both loops on ONE reactor, and neither through `SyncRun`: that function
    // resumes a coroutine exactly once and throws when it is still suspended, so a
    // driver awaiting `SleepUntil` aborted the process the first time anybody
    // started three nodes.
    //
    // The accept loop is submitted first, so a peer that dials the instant this
    // node's timers start finds somebody listening. The reverse order leaves a
    // window in which this node campaigns and refuses the votes it provoked.
    auto serve = [](Consensus::RaftPeerServer* server, ConsensusTier* tier) -> DetachedTask {
        co_await server->Run();
        tier->NoteLoopFinished();
        co_return;
    };
    auto tick = [](Consensus::RaftDriver* driver, IReactor* reactor, ConsensusTier* tier) -> DetachedTask {
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
    _transport->Start();

    serve(_peerServer.get(), this);
    tick(_driver.get(), &_reactor, this);

    _ioThread = std::jthread { [this] { _reactor.Run(); } };

    // Third thread, and it is the one that can afford to be: it holds no socket and
    // does nothing at all in the ordinary case. What it may NOT do is run on either
    // of the other two -- a proposal is a durability write and a broadcast, and both
    // of those loops exist precisely to not be held up by one.
    _reconcileThread = std::jthread { [this](std::stop_token stop) {
        while (!stop.stop_requested())
        {
            Reconcile();

            // Interruptible, rather than a sleep this loop would have to wake from
            // on its own schedule. A stop that had to wait out a full interval makes
            // teardown look hung, which this repository has already paid for once as
            // a `systemctl stop` that escalated to SIGKILL.
            auto guard = std::unique_lock { _wakeMutex };
            (void) _wake.wait_for(guard, stop, ReconcileInterval, [&stop] { return stop.stop_requested(); });
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
    if (_transport != nullptr)
        _transport->Stop();
    if (_peerServer != nullptr)
        _peerServer->Shutdown();
    if (_driver != nullptr)
        _driver->Stop();

    // Asked to stop and then woken, in that order: `request_stop` is what the
    // predicate reads, and notifying before it would leave the loop re-checking a
    // flag nobody had set yet and going back to sleep for a full interval.
    _reconcileThread.request_stop();
    _wake.notify_all();
}

std::expected<Consensus::LogIndex, ConsensusError> ConsensusTier::Propose(Cluster::Command const& command)
{
    // Validated BEFORE it is proposed, which is the only place a change can be
    // refused: an entry is applied after it is committed, when there is nobody left
    // to report a failure to and no way to un-commit it.
    if (auto const allowed = Cluster::Validate(command); !allowed.has_value())
        return std::unexpected { allowed.error() };

    return _driver->Propose(Cluster::Encode(command), std::chrono::steady_clock::now());
}

Cluster::ClusterState ConsensusTier::ClusterState() const
{
    return _application.State();
}

std::expected<void, ConsensusError> ConsensusTier::ProposeToCluster(Cluster::Command const& command)
{
    return Propose(command).transform([](Consensus::LogIndex) {});
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
    // The LAST one stops the reactor. `IReactor::Run` returns with its timer heap
    // and its parked work exactly where they were, so stopping it while either loop
    // is still suspended would leave a coroutine frame nobody ever resumes and
    // nobody ever frees -- a leak a sanitizer reports and a long-lived process pays
    // for.
    if (_loopsRunning.fetch_sub(1, std::memory_order_acq_rel) == 1)
        _reactor.Stop();
}

void ConsensusTier::Reconcile()
{
    auto const state = _application.State();

    // Every node, leader or not, and BEFORE anything is proposed. A member the
    // cluster agreed to admit has to be dialable by everybody -- the leader
    // replicates to it and every other member sends it votes -- and a member
    // counted towards a quorum that nobody dials is a cluster that stops forming
    // one.
    // Snapshotted once for the whole pass. Two reads would be two different values
    // -- discovery lands on its own interval -- so a leader could dial one set and
    // propose from another.
    auto desired = std::vector<Cluster::DesiredMember> {};
    {
        auto const guard = std::unique_lock { _desiredMutex };
        desired = _desired;
    }

    LearnMembers(state, desired);

    // Only a leader may propose, and asking here rather than letting `Propose`
    // refuse is what keeps a follower from logging a `NotLeader` every interval for
    // as long as it is a follower -- which is most of a healthy cluster's life.
    if (!_leads.load(std::memory_order_relaxed))
        return;

    // Outside the lock, both the decision and the proposals: a proposal is a
    // durability write and a broadcast, and holding a lock across one would stall
    // whoever is discovering peers behind whoever is writing to a disk.
    for (auto const& command: Cluster::MembershipProposals(state, desired))
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
            // `--node-id` or `--raft-peer` that is not text before this process
            // starts (`ParseUtf8Text`, #155). Which is exactly why it is worth
            // being loud rather than fatal.
            if (SubjectOf(proposed.error().code) == RefusalSubject::Moment)
            {
                _logger.Logf(
                    LogLevel::Info, "cluster: cannot record {} right now: {}", command.key, proposed.error().context);
                return;
            }

            // Warn, and "never" rather than "right now", because the two want
            // different things from whoever reads them: one is a leader election
            // in progress and the other is a record that has to be corrected
            // before it can ever be agreed.
            _logger.Logf(
                LogLevel::Warn, "cluster: {} can never be recorded as it stands: {}", command.key, proposed.error().context);
            continue;
        }

        _logger.Logf(LogLevel::Info,
                     "cluster: recorded {} at {}{}",
                     command.key,
                     command.value,
                     command.schedulerEndpoint.empty() ? std::string {}
                                                       : std::format(", scheduler {}", command.schedulerEndpoint));
    }

    // Against the state read at the top, which is deliberately the APPLIED one: a
    // member reaches it only after its record has committed, so the address is
    // agreed before the id is counted. That ordering is what lets every other node
    // learn where the new member answers before it is asked to vote for anybody.
    ReconcileQuorum(state);
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

    for (auto const& member: state.members)
        learn(member.id, member.raftEndpoint);

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

    auto change = Cluster::NextQuorumChange(state, progress.members, _self.id, _bootstrapIds);
    if (!change.has_value())
        return;

    auto const proposed = _driver->ProposeMembership(*change, std::chrono::steady_clock::now());
    if (!proposed.has_value())
    {
        // One line and out, and deliberately WITHOUT the `SubjectOf` split the
        // proposal loop above uses. `ProposeMembership` answers
        // `InvalidConfiguration` for two conditions that are plainly moments -- a
        // change already in flight, and a member set equal to the current one -- so
        // classifying by code here would report "wait for it to commit" as permanent,
        // at Warn, every interval. That is issue #196; until it is split, this path
        // treats every refusal as the moment it usually is.
        //
        // The commonest reason is that leadership moved between the two, which is
        // not a fault.
        _logger.Logf(LogLevel::Info, "cluster: cannot change the quorum right now: {}", proposed.error().context);
        return;
    }

    _quorumProposedAt = *proposed;

    // Recorded together, because the pair is what the wait above is asked about: an
    // index without its term cannot say whether the proposal it names can still be
    // the one that lands.
    _quorumProposedIn = progress.term;
    _logger.Logf(
        LogLevel::Info, "cluster: proposing a quorum of {} member(s) at index {}", change->size(), _quorumProposedAt.value);
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
    _lastLeader = change.knownLeader;
    Republish();
}

void ConsensusTier::OnStateChanged(Cluster::ClusterState const& state)
{
    // The member set reaches the fleet's oracle from here, so admitting a peer and
    // serving it are one decision rather than two facts that can disagree.
    if (_onMembers)
        _onMembers(state.Endpoints());

    // And the leader's ADDRESS may have just arrived, which is a different answer
    // from the role this node already knew.
    Republish();
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
    _leads.store(scheduled == Distributed::SchedulerRole::Leader, std::memory_order_relaxed);

    if (_onRole)
        // .value, because Term is a distinct type here and a plain integer on the
        // wire: the token carries a number, and the type exists to stop terms being
        // confused with log indices inside consensus rather than outside it.
        _onRole(scheduled, leaderEndpoint, _lastTerm.value);
}

std::expected<std::unique_ptr<ConsensusTier>, std::string> StartConsensusOrExplain(
    NodeConfig const& cfg,
    std::unique_ptr<SchedulerTier> const& schedulerTier,
    std::string_view schedulerBound,
    NodeMembership& membership,
    ILogger& logger)
{
    // No cluster configured, which is the common deployment: one machine, leading
    // itself. Requiring an operator to configure a one-member cluster to get that
    // would be ceremony for the ordinary case.
    if (cfg.nodeId.empty())
        return std::unique_ptr<ConsensusTier> {};

    auto tier = ConsensusTier::Start(
        cfg,
        schedulerBound,
        [&schedulerTier](Distributed::SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t term) {
            // Null when this node runs no scheduler surface, which is a legitimate
            // shape: a member that contributes CPU and consensus without handing out
            // anybody's work. It still votes, and its leadership -- if it wins -- is
            // simply not exercised through a port nobody can reach.
            if (schedulerTier != nullptr)
                schedulerTier->SetRole(role, leaderEndpoint, term);
        },
        [&membership](std::vector<std::string> const& endpoints) {
            // The replicated member set joins the fleet's admission policy, so a node
            // the cluster agreed to admit is served by every surface at once. It does
            // not *become* that policy: `--fleet-member` answers a different question
            // -- who may spend this node's CPU, clients included -- and survives every
            // commit (#251).
            membership.Publish(endpoints);
        },
        logger);

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
