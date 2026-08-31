// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Distributed/FleetSample.hpp>
#include <FastCache/Distributed/IClusterAdmin.hpp>
#include <FastCache/Distributed/LeaseTable.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache::Distributed
{

/// Whether this node currently speaks for the cluster.
///
/// An `enum class` rather than the `bool` a leadership check naturally returns,
/// because the third state already exists: during an election nobody leads, and
/// `Candidate` is neither "yes" nor a mistake. It is spelled apart from
/// `Consensus::Role` deliberately -- that type describes what the *consensus*
/// algorithm thinks, this one what the *scheduler* is allowed to do, and a node
/// that leads but has not yet caught up on its own log is the case where the two
/// answers differ.
/// The scheduler term a node leading alone is in.
///
/// Zero, and it is an answer rather than a placeholder: a node with no `--node-id`
/// runs no consensus, so there is no election and no term to be in. Named so that no
/// caller spells a bare `0` for it -- the term goes inside every lease grant (#322),
/// and a literal at a call site is exactly how somebody later reads it as "unknown"
/// and starts treating it as one.
inline constexpr std::uint64_t StandaloneSchedulerTerm = 0;

enum class SchedulerRole : std::uint8_t
{
    /// Not the leader. Refuse and redirect if a leader is known.
    Follower = 0,
    /// No leader is known -- an election is in progress.
    Undecided,
    /// This node leads and may hand out capacity.
    Leader,
    Last, ///< Not a role, and has no row: the length of a table keyed by one.
};

/// Whether the peer asking has earned the fleet's capacity.
///
/// The anti-leeching decision, named for what it *is* rather than for the check
/// that produces it. Zero is `Outsider` so a default-constructed request cannot
/// accidentally be admitted: the direction a mistake has to fail in.
enum class Membership : std::uint8_t
{
    /// Not a cluster member. Still served the cache; never handed a worker.
    Outsider = 0,
    /// An authenticated member of this cluster.
    Member,
};

/// Everything the scheduler needs to know about the caller, gathered by the
/// transport before it asks.
///
/// A struct rather than three parameters because two of them are strings and the
/// remaining pair would be silently exchangeable at every call site -- and because
/// a fourth fact about a caller is exactly the kind of thing PR 8's resource-aware
/// scheduling adds.
struct CallerContext
{
    /// Is this peer a member of the cluster?
    Membership membership { Membership::Outsider };

    /// The peer's **host**, as the kernel reports it -- never a name the peer
    /// chose.
    ///
    /// This used to say "who the peer says it is, for logs; never trusted for a
    /// decision", and every clause of that was wrong. The transport fills it from
    /// `ISocket::PeerAddress()` and hands the *same* string to the membership
    /// oracle, so it is already the basis of the one decision on this surface that
    /// matters -- and it is the only fact here a caller cannot forge.
    ///
    /// Correcting it is not cosmetic. A comment saying this cannot be trusted is a
    /// comment telling the next person that tying a registration to where it came
    /// from is impossible, which is how #242 stayed open: the fact needed to check
    /// an endpoint was already in the struct, disclaimed.
    ///
    /// A bare host, never an endpoint: a peer dials from an **ephemeral source
    /// port**, so there is no port here to compare anything against.
    ///
    /// It can legitimately be empty -- `FormatPeerAddress` answers that for a peer
    /// whose family is unknown or whose `getpeername` failed -- so anything reading
    /// it must decide what an unnameable caller means rather than assume a host.
    std::string_view peerId {};
};

/// What the scheduler decided, in the vocabulary of the wire but not yet on it.
///
/// The service is deliberately I/O-free, exactly as `WorkerRegistry` and
/// `LeaseTable` beneath it are: it decides, and something else writes. That is what
/// lets every rule below -- leadership, membership, capacity, duplicate
/// suppression, expiry -- be a `ManualClock` unit test rather than a socket and a
/// sleep.
struct SchedulerReply
{
    /// `Ok` with a payload, or `Error` with a code.
    CompileCacheWire::Status status { CompileCacheWire::Status::Error };
    /// Meaningful only when `status` is `Error`.
    CompileCacheWire::ErrorCode error { CompileCacheWire::ErrorCode::MalformedFrame };
    /// Overrides the error table's default when non-empty; carries the leader's
    /// endpoint for `NotLeader`.
    std::string message {};
    /// Meaningful only when `status` is `Ok`.
    std::vector<std::byte> payload {};

    /// A successful reply carrying @p payload.
    ///
    /// A factory rather than an aggregate at each call site, because `error` has no
    /// meaning on this branch and spelling *some* code there reads at a glance as
    /// though something had gone wrong. There is deliberately no `None` enumerator
    /// to reach for instead: `ErrorCode` is the wire's vocabulary, and a value that
    /// never travels does not belong in it.
    /// @param payload The encoded result, empty for a verb that returns nothing.
    /// @return The reply.
    [[nodiscard]] static SchedulerReply Success(std::vector<std::byte> payload = {})
    {
        return SchedulerReply { .status = CompileCacheWire::Status::Ok,
                                .error = CompileCacheWire::ErrorCode::MalformedFrame,
                                .message = {},
                                .payload = std::move(payload) };
    }

    /// A refusal that does not reach the service at all.
    ///
    /// The decode failures belong here rather than to `SchedulerService::Refuse`,
    /// which counts what it refuses: a frame that never decoded is not a fleet
    /// condition, and the `RefusalTable` row for `MalformedFrame` says so with
    /// `std::nullopt` for exactly this reason.
    /// @param message Overrides the table default when non-empty.
    /// @return The refusal.
    [[nodiscard]] static SchedulerReply Malformed(std::string message = {})
    {
        return SchedulerReply { .status = CompileCacheWire::Status::Error,
                                .error = CompileCacheWire::ErrorCode::MalformedFrame,
                                .message = std::move(message),
                                .payload = {} };
    }
};

/// The fleet scheduler: who may compile where, and on whose behalf.
///
/// ## Why this is not in the cache daemon any more
///
/// It used to be, reached through a `Dispatch` role on one of `fastcached`'s
/// listeners. That made the cache daemon a *scheduler* as well as a store, and the
/// two have opposite deployment shapes: a cache is a shared piece of infrastructure
/// somebody operates, while a scheduler must live where the cluster's leadership
/// already lives, because handing out capacity is a decision only one node may make
/// at a time. Raft supplies exactly that, and Raft runs in the node. So the
/// scheduler moved to where the answer to "am I allowed to decide this?" is already
/// known, rather than being asked to invent it.
///
/// ## Every refusal ends in a local compile
///
/// This is the invariant the whole feature rests on and it is why *every* method
/// here returns a refusal rather than an error: the caller is holding the source
/// and has a working compiler, so distribution can only ever make a build faster,
/// never break it. Each refusal is a distinct code because they are different
/// *operator* problems -- an empty fleet is a misconfiguration, a busy one is
/// under-capacity, a non-member is a policy decision somebody made -- even though
/// the client answers all of them identically.
class SchedulerService
{
  public:
    /// @param clock Time source for registry expiry and lease timeouts; must
    ///        outlive the service.
    /// @param wallClock Where a grant's absolute expiry comes from; must outlive the
    ///        service. Separate from @p clock because a steady instant means nothing
    ///        on the machine that has to check it, and a lease is checked on another
    ///        machine by definition.
    /// @param metrics Counts the outcomes below; must outlive the service.
    /// @param logger Where the one observation this service reports goes; must
    ///        outlive the service. `NullLogger` where a caller does not want it.
    /// @param signingKey The cluster's pre-shared key, copied. **Empty is legal and
    ///        means unsigned grants** -- the boundary this surface had before signed
    ///        leases existed, which a single machine with no `--cluster-key-file`
    ///        still runs. It is not silent: the first unsigned grant says so in the
    ///        log, once.
    /// @param clusterId Which fleet this scheduler leads, copied. Goes inside every
    ///        grant's MAC, so a worker refuses a grant from a fleet that is not its
    ///        own even when both trust the same key (#322). **Empty is legal** and
    ///        means a node with no `--cluster-id`, which is the one-machine
    ///        deployment: a verifier that names none expects none.
    SchedulerService(IClock& clock,
                     IWallClock const& wallClock,
                     IMetricsSink& metrics,
                     ILogger& logger,
                     std::span<std::byte const> signingKey,
                     std::string_view clusterId);

    /// Where a node's handed-over history goes, or null to discard it.
    ///
    /// A setter rather than a constructor parameter for the reason `SetRole` is one:
    /// the sink lives in the admin surface, which is built after the scheduler it
    /// reads from. Null is the ordinary state for a node with no dashboard, and
    /// discarding is then correct rather than a failure -- nothing would ever read
    /// what was kept.
    /// @param sink Where to route it; must outlive this service.
    void SetHistorySink(IFleetHistorySink* sink) noexcept;

    /// Publish this node's current standing in the cluster.
    ///
    /// Called by the consensus driver whenever leadership moves. Kept as a setter
    /// rather than a constructor parameter because leadership is exactly the thing
    /// that changes while the object lives -- the documented carve-out to
    /// configuration-at-construction, not an exception to it.
    /// @param role What this node may do.
    /// @param leaderEndpoint Where the leader is, when one is known; empty otherwise.
    /// @param epoch The scheduler term this standing belongs to. It goes inside every
    ///        grant this node mints, so a token captured before an election is not
    ///        replayable after it (#322). Zero is the term of a node leading alone
    ///        with no consensus, which is a real deployment rather than a missing
    ///        answer -- and it is why this is a parameter rather than something the
    ///        service could derive: only the consensus driver knows the term, and a
    ///        node without one still mints grants.
    void SetRole(SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch);

    /// Give this scheduler a cluster to administer.
    ///
    /// A setter for the reason `SetRole` is one, and a different one: consensus
    /// cannot be constructed before the scheduler surface it drives, because it
    /// needs the port that surface bound in order to announce where clients reach
    /// this node. So the two are wired in the only order that exists.
    ///
    /// Left unset, the three cluster verbs are refused with `NoCluster` -- which
    /// is the honest answer for a single node started without `--node-id`: it
    /// leads itself and has no replicated state for anybody to change.
    /// @param admin The cluster; must outlive this service.
    void AdministerWith(IClusterAdmin& admin) noexcept
    {
        _admin = &admin;
    }

    /// This node's current standing.
    /// @return The role last published.
    [[nodiscard]] SchedulerRole Role() const noexcept
    {
        return _role.load(std::memory_order_acquire);
    }

    /// Admit a worker to the fleet.
    /// @param caller Who is asking.
    /// @param registration The worker's own description of itself.
    /// @return `Ok` carrying the assigned worker id, or a refusal.
    [[nodiscard]] SchedulerReply Register(CallerContext const& caller, WorkerRegistration const& registration);

    /// Refresh a worker's liveness and record what it says about itself.
    /// @param caller Who is asking.
    /// @param workerId The id handed back by `Register`.
    /// @param load The worker's own account of its job count and its machine.
    /// @return `Ok`, or a refusal.
    /// @param history Closed buckets this node is handing over, oldest first. Routed
    ///                 to the sink under the worker's ENDPOINT, and dropped when
    ///                 there is no sink.
    [[nodiscard]] SchedulerReply Heartbeat(CallerContext const& caller,
                                           std::string_view workerId,
                                           NodeLoad const& load,
                                           std::span<FleetBucket const> history = {});

    /// Pick a worker and authorize one job on it.
    ///
    /// The grant's token is a **credential**, not merely bookkeeping: it carries a
    /// MAC over the worker's endpoint, the toolchain, the object key and an expiry,
    /// so the worker can refuse a client that never came through here. Without the
    /// endpoint inside that MAC a grant issued for one machine would be a grant on
    /// every machine sharing the key -- the same failure `Cluster::DiscoveryWire`
    /// closes by covering the `(node, endpoint)` pair. See `LeaseToken.hpp`.
    /// @param caller Who is asking.
    /// @param request The toolchain, the object key and the client's codecs.
    /// @return `Ok` carrying an encoded `LeaseGrant`, or a refusal.
    [[nodiscard]] SchedulerReply Lease(CallerContext const& caller, CompileCacheWire::LeaseRequest const& request);

    /// Resolve a lease whose job has ended, however it ended.
    ///
    /// The transition that was missing for as long as there was no verb to carry
    /// it (#212). Without it a key stayed marked in-flight for the full lease
    /// timeout, so recompiling the same translation unit inside ten minutes of a
    /// dispatch was refused `AlreadyInFlight` and fell back to a local compile --
    /// with expiry, which the lease table documents as the safety net for a client
    /// that *died*, doing the work of the ordinary path.
    ///
    /// Answered by the CLIENT rather than by the worker, because the client is who
    /// the lease was issued to and is the only party that sees every way a job can
    /// end: a dispatch that never reached its worker is invisible to that worker.
    ///
    /// An unknown token is refused rather than waved through, and the refusal is
    /// the diagnostic: it means the lease had already expired under this job, which
    /// is an operator telling their fleet that `DefaultLeaseTimeout` is shorter
    /// than their slowest translation unit.
    ///
    /// The key is stated too, and a token naming a different one resolves nothing:
    /// `LeaseTable` mints tokens sequentially, so the pair is what makes this the
    /// caller's own lease rather than whichever the number happens to land on --
    /// after a scheduler restart, or from a member guessing.
    ///
    /// The token is the SIGNED grant, so it is authenticated here and the serial the
    /// lease table knows is unwrapped out of it. That closes the guessing half of the
    /// residual this used to record: a member could once resolve somebody else's
    /// lease by naming a key and a small integer, and a small integer is not a
    /// secret. It does not close replay by whoever legitimately holds the grant --
    /// nothing short of per-client identity could -- and what that still costs is one
    /// duplicated compile and one premature decrement the next heartbeat corrects.
    ///
    /// A scheduler with no `--cluster-key-file` signs nothing, so there is nothing to
    /// unwrap and the token is the serial, exactly as before.
    /// @param caller Who is asking.
    /// @param leaseToken The token this client was granted.
    /// @param key The object key it was granted on.
    /// @return `Ok`, or a refusal.
    [[nodiscard]] SchedulerReply Release(CallerContext const& caller, std::string_view leaseToken, std::string_view key);

    /// Report what the cluster has agreed.
    ///
    /// Behind the same gate as everything else here, which for a *read* is worth
    /// stating: a follower's copy is perfectly valid and merely older, so this
    /// could have answered from any member. Refusing and naming the leader keeps
    /// one rule for the whole surface -- a verb added without the gate is the
    /// regression the arrangement exists to make impossible -- and it sends the
    /// operator to the node they would need anyway to change anything.
    /// @param caller Who is asking.
    /// @return `Ok` carrying the encoded state, or a refusal.
    [[nodiscard]] SchedulerReply ClusterStatus(CallerContext const& caller);

    /// Change one replicated setting.
    /// @param caller Who is asking.
    /// @param name A key from `Cluster::SettingTable`.
    /// @param value What to set it to.
    /// @return `Ok` once the entry is appended, or a refusal.
    [[nodiscard]] SchedulerReply ClusterSet(CallerContext const& caller, std::string_view name, std::string_view value);

    /// Remove a member from the cluster.
    ///
    /// The one membership change nothing automatic makes: discovery only ever
    /// adds, because a peer vanishes from a broadcast far more often than it
    /// leaves. Removing is an operator's decision and this is where they make it.
    /// @param caller Who is asking.
    /// @param memberId Who to remove.
    /// @return `Ok` once the entry is appended, or a refusal.
    [[nodiscard]] SchedulerReply ClusterForget(CallerContext const& caller, std::string_view memberId);

    /// Add a member to the cluster, or record that one has moved.
    ///
    /// The counterpart `ClusterForget` had none of, and its absence was the reason
    /// a fleet without `--discovery` could shrink and never grow: nothing anywhere
    /// could put a member into the replicated state, so `--raft-peer` stayed the
    /// only answer and growing a cluster meant restarting every machine in it.
    ///
    /// One verb for adding and for moving, because they are one intention — a node
    /// that moved has the same identity and a new address, and making an operator
    /// remove it first would leave a window in which the cluster has agreed it does
    /// not exist. The scheduler endpoint is not a parameter: a member announces its
    /// own once elected, and a value typed here about somebody else would be a
    /// guess that outranks what they say about themselves.
    /// @param caller Who is asking.
    /// @param memberId The member's identity.
    /// @param raftEndpoint host:port its consensus port answers on.
    /// @return Accepted, or why it was refused.
    [[nodiscard]] SchedulerReply ClusterAdmit(CallerContext const& caller,
                                              std::string_view memberId,
                                              std::string_view raftEndpoint);

    /// Where the leader answers, when one is known.
    ///
    /// The scheduler endpoint rather than the consensus one, and that distinction
    /// is a defect this pairing already closed once: a follower that names the
    /// leader's Raft port sends a client to a socket which speaks nothing it
    /// understands. It is the same string `NotLeader` carries in its message, and
    /// empty for `Undecided` -- an election in progress has nobody to name.
    /// @return The leader's scheduler endpoint, or empty.
    [[nodiscard]] std::string LeaderEndpoint() const
    {
        std::scoped_lock const guard { _leaderMutex };
        return _leaderEndpoint;
    }

    /// What is outstanding right now: the oldest few, and how many there are.
    ///
    /// Narrow on purpose: a reporting caller has no business holding a
    /// `LeaseTable&`, which is mutable and whose other verbs account for capacity.
    /// Bounded for a reason of its own -- a busy fleet holds thousands, so a report
    /// shows the oldest few beside the total rather than all of them, and which end
    /// is not a presentation choice: the oldest are the leases that have stopped
    /// moving.
    /// @param limit How many entries to return at most; zero asks only for the total.
    /// @return The listing, both halves sampled together.
    [[nodiscard]] LeaseListing OutstandingLeases(std::size_t limit) const
    {
        return _leases.LiveLeases(limit);
    }

    /// The registry, for the admin endpoint and for tests.
    [[nodiscard]] WorkerRegistry const& Workers() const noexcept
    {
        return _workers;
    }

    /// The lease table, for the worker's own validation path and for tests.
    [[nodiscard]] LeaseTable& Leases() noexcept
    {
        return _leases;
    }

  private:
    /// Where handed-over history goes; null until the admin surface sets one.
    IFleetHistorySink* _history { nullptr };

    /// What a verb is asking for, which decides how much of the gate applies.
    ///
    /// **The distinction is between deciding and settling**, and it is worth stating
    /// exactly because the same argument will be asked of every verb added later.
    ///
    /// Almost everything this surface does is a decision about the fleet's FUTURE:
    /// who joins it, what it is worth scheduling next, what the cluster agrees.
    /// A node that is not the leader must make none of those, because a demoted node
    /// answering them is how a split fleet forms -- its registry is a stale copy of
    /// somebody else's, so a worker admitted there heartbeats happily into a fleet
    /// nothing schedules onto.
    ///
    /// A release is not that. It settles an obligation **this node itself created**,
    /// against a record **only this node holds**: `LeaseTable` is per-node and is
    /// never replicated, so the lease exists nowhere else and no other node can free
    /// it. Refusing it does not stop a demoted node from doing harm -- it stops it
    /// from cleaning up after itself, and the key stays pinned until it expires
    /// ([#371](https://github.com/LASTRADA-Software/fastcached/issues/371)).
    ///
    /// So the test for a future verb is not "does this feel administrative" but a
    /// checkable one: **does it act only on non-replicated state this node created
    /// itself, and can it create nothing?** `Release` can erase one entry it minted
    /// and decrement its own speculative in-flight count, and that is all it can do.
    /// Everything else here fails that test, `Heartbeat` included -- a heartbeat
    /// refreshes the registry the LEADER schedules from, so a demoted node's copy
    /// going stale is the mechanism that pushes a worker to re-register with the new
    /// leader rather than a cost.
    enum class GateScope : std::uint8_t
    {
        /// A decision about the fleet's future. Leadership and membership both.
        Scheduling = 0,
        /// The settlement of an obligation this node already created. Membership only.
        Settlement,
    };

    /// The gates a verb passes, in the order that costs least to answer.
    ///
    /// One function rather than a pair of checks repeated at every verb: these are
    /// the security- and policy-relevant decisions of the whole surface, and a verb
    /// added without them would otherwise be a verb that quietly skips both.
    ///
    /// The scope defaults to `Scheduling` so a verb added without thinking about it
    /// gets the strict gate. Relaxing it has to be typed out, which is the direction
    /// that fails safe.
    ///
    /// **Membership is never relaxed.** The two rules answer different questions --
    /// *may this node decide* and *may this caller spend the fleet's CPU* -- and only
    /// the first stops applying at demotion. A non-member must not reach the lease
    /// table whoever leads.
    /// @param caller Who is asking.
    /// @param scope What they are asking for.
    /// @return A refusal, or nullopt when the caller may proceed.
    [[nodiscard]] std::optional<SchedulerReply> Gate(CallerContext const& caller,
                                                     GateScope scope = GateScope::Scheduling) const;

  public:
    /// The half of `Gate()` that depends on the CALLER alone.
    ///
    /// Public and named because the transport asks it **before it reads a payload**:
    /// membership is a property of the peer, so a stranger can be refused without
    /// this process allocating for the frame it declared
    /// ([#285](https://github.com/LASTRADA-Software/fastcached/issues/285)).
    ///
    /// **`Gate()` calls this rather than repeating it, and that is the point.** Two
    /// call sites for one rule is how the two come to disagree -- an edit to what
    /// membership means landing in the early check and not the authoritative one
    /// would leave a door open that every test still says is shut. There is one
    /// function, so there is one answer.
    ///
    /// It carries the counter with it, for the same reason: a refusal's wire code and
    /// its counter are one fact, and separating them is how a gate comes to work
    /// while the dashboard says it never fired. Each call increments once -- the
    /// transport refusing early does not then also call `Answer`, so one refused
    /// request moves it exactly one.
    ///
    /// Leadership is deliberately NOT here. It is a fact about this node rather than
    /// about the caller, and its refusal carries the leader's endpoint that clients
    /// and workers both follow -- so hoisting it would put a change to redirect
    /// behaviour inside a change about buffering.
    /// @param caller Who is asking.
    /// @return A refusal, or nullopt when the caller is admitted.
    [[nodiscard]] std::optional<SchedulerReply> RefuseUnlessMember(CallerContext const& caller) const;

  private:
    /// Drop workers that stopped heartbeating, and free what they were holding.
    ///
    /// The registry and the lease table are siblings and neither may reach the
    /// other, so the pairing lives here -- which is also the only place that knows
    /// a dropped worker's leases were about a machine that is gone rather than a
    /// job that finished.
    ///
    /// Run from `Lease` and nowhere else, because that is the one decision a stale
    /// worker's leftover leases corrupt: duplicate suppression asks about the key
    /// first, so a machine that vanished mid-job made every later client miss on
    /// one of its keys fall back to a local compile until the lease timed out. On
    /// a live fleet it walks the registry and drops nothing, which is the same
    /// walk `Pick` does immediately afterwards.
    void ReapExpiredWorkers();

    /// Build a refusal, counting it when the table names a counter.
    /// @param code Why.
    /// @param message Overrides the table default when non-empty.
    /// @return The refusal.
    [[nodiscard]] SchedulerReply Refuse(CompileCacheWire::ErrorCode code, std::string message = {}) const;

    /// Offer a validated command to the cluster and render what came back.
    ///
    /// One place rather than one per verb, because the mapping from a consensus
    /// refusal to a wire code is the same for every change and is a table.
    /// @param command The change.
    /// @return `Ok`, or the refusal with its reason.
    [[nodiscard]] SchedulerReply Offer(Cluster::Command const& command);

    /// Mint the token a grant hands back.
    ///
    /// A function rather than an expression inside `Lease`, because it is where the
    /// unsigned fallback lives and that decision deserves a name. Without a key it
    /// returns the bare `LeaseTable` serial -- exactly what this surface handed out
    /// before signed leases -- and says so in the log the first time.
    /// @param lease The lease just acquired.
    /// @param endpoint The worker it was granted on.
    /// @param fingerprint The toolchain it was granted against.
    /// @return What the client presents to that worker.
    [[nodiscard]] std::string MintGrantToken(Distributed::Lease const& lease,
                                             std::string_view endpoint,
                                             std::string_view fingerprint);

    IWallClock const& _wallClock;
    IMetricsSink& _metrics;
    /// Where the endpoint-mismatch observation goes (#242).
    ///
    /// A reference at construction rather than a nullable setter, because unlike
    /// `SetHistorySink` and `SetRole` -- which exist for collaborators that arrive
    /// later or change -- every caller already holds a logger when it builds this.
    /// `NullLogger` is how a caller says it wants none, which is a decision somebody
    /// makes rather than a pointer somebody forgot.
    ILogger& _logger;

    /// Mismatch lines already written, so the diagnostic is bounded.
    ///
    /// Atomic defensively rather than because it is currently shared: today the
    /// scheduler port is one `FrameEndpoint` on one reactor thread, and `_workers`
    /// and `_leases` beside it are plain members that assume exactly that -- so a
    /// second thread answering registrations would be a bigger change than this
    /// field. Relaxed because nothing is ordered against it: an exact cut-off is not
    /// the point, and the counter beside it is what carries the rate anyway.
    std::atomic<std::uint64_t> _mismatchLines { 0 };

    /// Whether the "these grants are unsigned" line has already been written.
    ///
    /// One line for the life of the process, not one per grant: the fact is about
    /// the configuration and does not change, and a per-grant line would bury it
    /// under itself on the first parallel build. Atomic for the reason
    /// `_mismatchLines` is -- defensively, against a second thread ever answering
    /// this surface.
    std::atomic<bool> _warnedUnsigned { false };

    /// The cluster's pre-shared key, or empty for unsigned grants.
    ///
    /// Copied rather than a `std::span` at the caller's buffer: the key is read from
    /// a file by a tier that has no reason to outlive this service, and a signing key
    /// that quietly becomes a dangling view is the kind of defect that authenticates
    /// nothing while every test passes.
    std::vector<std::byte> _signingKey;

    /// Which fleet this scheduler leads; empty when the operator named none.
    ///
    /// Copied for the reason the key is: it is read from a configuration that has no
    /// reason to outlive this service.
    std::string _clusterId;

    WorkerRegistry _workers;
    LeaseTable _leases;

    /// This node's standing, and where the leader is.
    ///
    /// Written by the consensus driver whenever leadership moves, and read by
    /// every thread that serves a verb -- and now by whichever thread renders a
    /// fleet report, which is what made the race worth closing rather than
    /// reasoning about. The role is atomic and the endpoint is behind a mutex
    /// because one is a byte and the other reallocates.
    ///
    /// They are read *separately*, so a report can in principle catch a new role
    /// beside an old endpoint. That is deliberate rather than overlooked: the same
    /// interleaving is reachable by a heartbeat landing one instant later, so a
    /// lock spanning both would buy an atomicity the fleet does not have anyway.
    std::atomic<SchedulerRole> _role { SchedulerRole::Undecided };

    /// The term the role above belongs to, stamped into every grant minted under it.
    ///
    /// Atomic and read on the minting path rather than guarded beside the endpoint:
    /// a grant carrying the term from one instant either side of an election is
    /// exactly as correct as one minted an instant earlier or later, and the token's
    /// own expiry is what bounds it. What must not happen is a torn read, which is
    /// what makes this atomic rather than a plain member.
    std::atomic<std::uint64_t> _epoch { 0 };

    mutable std::mutex _leaderMutex;
    std::string _leaderEndpoint {}; ///< Guarded by `_leaderMutex`.

    /// The cluster, when this node runs one. Null is a legitimate state.
    IClusterAdmin* _admin { nullptr };
};

} // namespace FastCache::Distributed
