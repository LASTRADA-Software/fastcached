// SPDX-License-Identifier: Apache-2.0
#include "ConsensusTier.hpp"

#include <FastCache/Async/PlatformReactor.hpp>
#include <FastCache/Core/Clock.hpp>
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

    /// What to call a role in a log line.
    ///
    /// A table rather than a conditional chain, which is what clang-tidy's
    /// `readability-avoid-nested-conditional-operator` is really asking for and what
    /// this codebase asks for anyway: a fourth role is a row here rather than another
    /// `?:` somebody threads through an existing expression.
    constexpr std::array RoleNames {
        std::string_view { "a follower" }, // Follower = 0
        std::string_view { "undecided" },  // Undecided
        std::string_view { "the leader" }, // Leader
    };

    static_assert(RoleNames.size() == static_cast<std::size_t>(Distributed::SchedulerRole::Leader) + 1,
                  "RoleNames must hold one row per SchedulerRole, in enumerator order");

    /// @param role The role.
    /// @return Its name.
    [[nodiscard]] constexpr std::string_view RoleName(Distributed::SchedulerRole role) noexcept
    {
        return RoleNames[static_cast<std::size_t>(role)];
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

/// One `id=host:port` peer specification, parsed.
///
/// A free function rather than a lambda so the rule can be tested: `NodeConfig_test`
/// reaches it, and `main.cpp` -- where this would otherwise live -- is in no test
/// target.
/// @param spec The token as an operator wrote it.
/// @return The member, or nullopt when it is not one.
std::optional<Cluster::ClusterMember> ParsePeerSpec(std::string_view spec)
{
    // Split at the FIRST `=`, so an endpoint may contain one and an id may not. The
    // other way round would make `n1=host=1:6675` parse as an id of `n1=host`, which
    // is an id no operator wrote and which would silently never match a vote.
    auto const split = spec.find('=');
    if (split == std::string_view::npos)
        return std::nullopt;

    auto const id = spec.substr(0, split);
    auto const endpoint = spec.substr(split + 1);
    if (id.empty() || endpoint.empty())
        return std::nullopt;

    // The endpoint must be one a peer can dial, which is what `ParseEndpoint` with no
    // default host answers: a bare port names no machine, and a member recorded that
    // way is one the cluster counts towards quorum and cannot reach.
    if (!SplitHostPort(endpoint).has_value())
        return std::nullopt;

    // Every field named, including the one this token cannot carry. A member's
    // scheduler endpoint is a port peers never connect to, so nothing an operator
    // types about a PEER could supply it -- the node announces its own. Saying so
    // with `{}` rather than leaving it out is what keeps a field added to the
    // middle of the struct from becoming a silent zero here.
    return Cluster::ClusterMember { .id = std::string { id },
                                    .raftEndpoint = std::string { endpoint },
                                    .schedulerEndpoint = {} };
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
    std::vector<Cluster::ClusterMember> members;
    for (auto const& spec: cfg.raftPeers)
    {
        auto parsed = ParsePeerSpec(spec);
        if (!parsed.has_value())
            return std::unexpected { std::format("--raft-peer={} is not <id>=<host>:<port>", spec) };
        members.push_back(*std::move(parsed));
    }

    auto const self = std::ranges::find(members, cfg.nodeId, &Cluster::ClusterMember::id);
    if (self == members.end())
        return std::unexpected { std::format("--node-id={} names no --raft-peer; this node must be a member of its own "
                                             "cluster, with the endpoint its peers dial",
                                             cfg.nodeId) };

    // The wildcard for a bare port, like the scheduler's and unlike the cache's:
    // peers are on other machines by definition, so a loopback default would be one
    // that silently cannot work.
    auto const endpoint = ParseEndpoint(cfg.raftListen, "0.0.0.0");
    if (!endpoint.has_value())
        return std::unexpected { std::format("--listen-raft={} names no usable port", cfg.raftListen) };

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
                                                                     std::format("{}:{}", endpoint->first, endpoint->second),
                                                                     std::move(onRole),
                                                                     std::move(onMembers),
                                                                     logger } };

    if (auto started = tier->Launch(cfg, members, endpoint->first, endpoint->second); !started.has_value())
        return std::unexpected { started.error() };

    logger.Logf(LogLevel::Info,
                "consensus on {} as {} ({} member(s), state in {})",
                tier->BoundEndpoint(),
                cfg.nodeId,
                members.size(),
                stateDirectory.string());
    return tier;
}

std::expected<void, std::string> ConsensusTier::Launch(NodeConfig const& cfg,
                                                       std::vector<Cluster::ClusterMember> const& members,
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

    std::vector<Consensus::PeerEndpoint> peers;
    std::vector<Consensus::NodeId> ids;
    peers.reserve(members.size());
    ids.reserve(members.size());
    for (auto const& member: members)
    {
        ids.push_back(member.id);
        // Already checked by `ParsePeerSpec`, so this cannot fail -- but it is split
        // again rather than carried, because carrying it would mean the parsed form
        // and the string could disagree about which of a v6 address's colons is the
        // port separator, which is the defect `Core/HostPort` exists to hold in one
        // place.
        auto const split = SplitHostPort(member.raftEndpoint);
        if (!split.has_value())
            return std::unexpected { std::format("{} is not a dialable endpoint for {}", member.raftEndpoint, member.id) };
        auto const port = ParseTcpPort(split->second);
        if (!port.has_value())
            return std::unexpected { std::format("{} names no usable port for {}", member.raftEndpoint, member.id) };

        // This node itself is deliberately included. `RaftPeerTransport` refuses a
        // message addressed to `self` rather than looping it through a socket, so
        // filtering here would duplicate a rule it already enforces -- and doing it
        // in two places is how they come to disagree about which node is which.
        peers.push_back(Consensus::PeerEndpoint { .id = member.id, .host = split->first, .port = *port });
    }

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
    _driver->ObserveRole([this](Consensus::Role role, std::optional<Consensus::NodeId> const& knownLeader) {
        PublishRole(role, knownLeader);
    });

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
    // Only a leader may propose, and asking here rather than letting `Propose`
    // refuse is what keeps a follower from logging a `NotLeader` every interval for
    // as long as it is a follower -- which is most of a healthy cluster's life.
    if (!_leads.load(std::memory_order_relaxed))
        return;

    auto desired = std::vector<Cluster::DesiredMember> {};
    {
        auto const guard = std::unique_lock { _desiredMutex };
        desired = _desired;
    }

    // Outside the lock, both the decision and the proposals: a proposal is a
    // durability write and a broadcast, and holding a lock across one would stall
    // whoever is discovering peers behind whoever is writing to a disk.
    for (auto const& command: Cluster::MembershipProposals(_application.State(), desired))
    {
        auto const proposed = Propose(command);
        if (!proposed.has_value())
        {
            // One line and out. The commonest reason is that leadership moved
            // between the check above and here, which is not a fault and is not
            // repaired by trying the rest of the list against the same lost term.
            _logger.Logf(LogLevel::Info, "cluster: cannot record {} right now: {}", command.key, proposed.error().context);
            return;
        }

        _logger.Logf(LogLevel::Info,
                     "cluster: recorded {} at {}{}",
                     command.key,
                     command.value,
                     command.schedulerEndpoint.empty() ? std::string {}
                                                       : std::format(", scheduler {}", command.schedulerEndpoint));
    }
}

void ConsensusTier::PublishRole(Consensus::Role role, std::optional<Consensus::NodeId> const& knownLeader)
{
    _lastRole = role;
    _lastLeader = knownLeader;
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
    if (_published && scheduled == _publishedRole && leaderEndpoint == _publishedEndpoint)
        return;

    _published = true;
    _publishedRole = scheduled;
    _publishedEndpoint = leaderEndpoint;
    _leads.store(scheduled == Distributed::SchedulerRole::Leader, std::memory_order_relaxed);

    _logger.Logf(LogLevel::Info,
                 "consensus: this node is now {}{}",
                 RoleName(scheduled),
                 leaderEndpoint.empty() ? std::string {} : std::format(" of {}", leaderEndpoint));

    if (_onRole)
        _onRole(scheduled, leaderEndpoint);
}

std::expected<std::unique_ptr<ConsensusTier>, std::string> StartConsensusOrExplain(
    NodeConfig const& cfg, std::unique_ptr<SchedulerTier> const& schedulerTier, NodeMembership& membership, ILogger& logger)
{
    // No cluster configured, which is the common deployment: one machine, leading
    // itself. Requiring an operator to configure a one-member cluster to get that
    // would be ceremony for the ordinary case.
    if (cfg.nodeId.empty())
        return std::unique_ptr<ConsensusTier> {};

    // What the surface BOUND rather than what was asked for, because `--listen-
    // scheduler=0` means "pick a port" and an endpoint echoing `:0` back names
    // nothing a client could dial.
    auto const schedulerBound = schedulerTier != nullptr ? schedulerTier->BoundEndpoint() : std::string {};

    auto tier = ConsensusTier::Start(
        cfg,
        schedulerBound,
        [&schedulerTier](Distributed::SchedulerRole role, std::string_view leaderEndpoint) {
            // Null when this node runs no scheduler surface, which is a legitimate
            // shape: a member that contributes CPU and consensus without handing out
            // anybody's work. It still votes, and its leadership -- if it wins -- is
            // simply not exercised through a port nobody can reach.
            if (schedulerTier != nullptr)
                schedulerTier->SetRole(role, leaderEndpoint);
        },
        [&membership](std::vector<std::string> const& endpoints) {
            // The replicated member set becomes the fleet's admission policy, so a
            // node the cluster agreed to admit is served by every surface at once.
            // `--fleet-member` is the bootstrap answer; this is the running one.
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
