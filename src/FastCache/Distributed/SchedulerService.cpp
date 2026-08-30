// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cluster/ClusterIdentity.hpp>
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// One row per refusal that moves a counter: the wire code, and which one.
    ///
    /// A table rather than a `Count(...)` call beside each `Refuse(...)`, for the
    /// reason the worker's own version records: a refusal that forgets its counter
    /// is invisible in exactly the situation an operator is trying to diagnose, and
    /// hand-written pairs are as many chances to forget one.
    ///
    /// Both fields are plain and neither carries a default member initializer, which
    /// is the same rule the worker's `RefusalTable` states and for the same reason: a
    /// row answering one of the two questions is not a row, and `ErrorCode` has no
    /// zero enumerator for `{}` to name in the first place.
    struct RefusalDescriptor
    {
        Wire::ErrorCode code;          ///< The refusal.
        IMetricsSink::Counter counter; ///< What the operator sees rise.
    };

    constexpr std::array RefusalTable {
        RefusalDescriptor { .code = Wire::ErrorCode::NoWorker, .counter = IMetricsSink::Counter::DispatchLeasesNoWorker },
        RefusalDescriptor { .code = Wire::ErrorCode::NoCapacity,
                            .counter = IMetricsSink::Counter::DispatchLeasesNoCapacity },
        RefusalDescriptor { .code = Wire::ErrorCode::Withdrawn, .counter = IMetricsSink::Counter::DispatchLeasesWithdrawn },
        RefusalDescriptor { .code = Wire::ErrorCode::AlreadyInFlight,
                            .counter = IMetricsSink::Counter::DispatchLeasesDuplicate },
        RefusalDescriptor { .code = Wire::ErrorCode::MalformedRegistration,
                            .counter = IMetricsSink::Counter::DispatchWorkerRegistrationsMalformed },
        // Counted, unlike `UnknownLease` beside it, and the split is the diagnostic:
        // that one is a lease this scheduler issued and has since forgotten, which is
        // a statement about one client's timing. This one was never issued at all, so
        // a rise is somebody forging releases -- or, far more likely, a launcher from
        // before signed leases still handing back the bare serial it was given.
        RefusalDescriptor { .code = Wire::ErrorCode::LeaseUnauthorized,
                            .counter = IMetricsSink::Counter::DispatchLeasesUnauthorized },
    };

    /// The refusals this service makes that deliberately move nothing.
    ///
    /// Named rather than merely absent, so "this code has no counter" and "somebody
    /// forgot to give this code a counter" are different states rather than the same
    /// silence. Each is a *client* or *cluster* condition rather than a fleet one: a
    /// malformed frame is a broken client, an unknown worker id is a scheduler that
    /// restarted, and the two gate refusals are policy answers -- counting any of them
    /// beside the capacity refusals would put noise into the numbers a fleet is sized
    /// from, which is the very split those counters exist to preserve.
    constexpr std::array UncountedRefusals { Wire::ErrorCode::NotLeader,
                                             Wire::ErrorCode::NotAMember,
                                             Wire::ErrorCode::MalformedFrame,
                                             Wire::ErrorCode::UnknownLease,
                                             // Cluster administration: an operator typed something, and what
                                             // they typed is not a fact about fleet capacity. Counting these
                                             // beside the lease refusals would put one person's typo into the
                                             // numbers a fleet is sized from.
                                             Wire::ErrorCode::NoCluster,
                                             Wire::ErrorCode::InvalidClusterChange,
                                             Wire::ErrorCode::StorageWriteFailed };

    /// Whether every refusal this service can produce is accounted for exactly once.
    ///
    /// The completeness check the two tables exist to make possible: a refusal added
    /// to neither, or to both, is a build failure rather than a counter an operator
    /// discovers is missing while diagnosing a fleet.
    /// @return True when the two tables are disjoint.
    [[nodiscard]] consteval bool RefusalsAreDisjoint() noexcept
    {
        for (auto const& row: RefusalTable)
            for (auto const uncounted: UncountedRefusals)
                if (row.code == uncounted)
                    return false;
        return true;
    }

    static_assert(RefusalsAreDisjoint(), "a refusal either moves a counter or is listed as moving none, never both");

    /// What a consensus refusal becomes on the wire.
    struct ProposalRefusalRow
    {
        ConsensusErrorCode code;  ///< What consensus said.
        Wire::ErrorCode reported; ///< What the client is told.
    };

    /// One row per `ConsensusErrorCode`, in enumerator order: the wire code it
    /// becomes when a proposal is refused.
    ///
    /// A table for the reason the two above are: a `switch` here and a `switch`
    /// somewhere else drift, and a refusal reported under the wrong code sends an
    /// operator to fix something that was never wrong. The last three rows are
    /// peer-wire decode failures that a *local* proposal cannot produce -- they
    /// exist because a reader disagreed with a sender about some bytes, and no
    /// bytes are involved here -- so they map to the generic refusal, which is the
    /// closed-by-default answer rather than a claim about what happened.
    constexpr EnumTable<ConsensusErrorCode, ProposalRefusalRow> ProposalRefusals { {
        { .code = ConsensusErrorCode::InvalidConfiguration, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::NotLeader, .reported = Wire::ErrorCode::NotLeader },
        { .code = ConsensusErrorCode::StorageFailure, .reported = Wire::ErrorCode::StorageWriteFailed },
        { .code = ConsensusErrorCode::MalformedFrame, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::UnknownMessageType, .reported = Wire::ErrorCode::InvalidClusterChange },
        { .code = ConsensusErrorCode::UnsupportedVersion, .reported = Wire::ErrorCode::InvalidClusterChange },
    } };

    static_assert(RowsInEnumeratorOrder(ProposalRefusals, &ProposalRefusalRow::code),
                  "ProposalRefusals must hold one row per ConsensusErrorCode, in enumerator order");

    /// The wire code a consensus refusal is reported as.
    /// @param code What consensus said.
    /// @return The wire code.
    [[nodiscard]] constexpr Wire::ErrorCode WireCodeFor(ConsensusErrorCode code) noexcept
    {
        return ProposalRefusals[static_cast<std::size_t>(code)].reported;
    }

    /// What a pick refusal becomes on the wire.
    struct PickErrorRow
    {
        PickError error;          ///< Why no worker could be chosen.
        Wire::ErrorCode reported; ///< What the client is told.
    };

    /// One row per `PickError`, in enumerator order: the wire code it becomes.
    ///
    /// A table rather than a conditional for the reason the refusal table below is
    /// one: the mapping is the whole of what a client and an operator are told, and
    /// a `PickError` added without a row here would silently arrive as whichever
    /// arm an `if` happened to fall through to.
    constexpr EnumTable<PickError, PickErrorRow> PickErrorTable { {
        { .error = PickError::NoWorker, .reported = Wire::ErrorCode::NoWorker },
        { .error = PickError::NoCapacity, .reported = Wire::ErrorCode::NoCapacity },
        { .error = PickError::Withdrawn, .reported = Wire::ErrorCode::Withdrawn },
    } };

    static_assert(RowsInEnumeratorOrder(PickErrorTable, &PickErrorRow::error),
                  "PickErrorTable must hold one row per PickError, in enumerator order");

    /// The wire code a pick refusal is reported as.
    /// @param error Why no worker could be chosen.
    /// @return The code to answer with.
    [[nodiscard]] constexpr Wire::ErrorCode WireCodeFor(PickError error) noexcept
    {
        return PickErrorTable[static_cast<std::size_t>(error)].reported;
    }

    /// Every string a REGISTER carries, in one place.
    ///
    /// A table rather than three checks written out, because the failure this
    /// guards against is a FOURTH string being added to `WorkerRegistration` and
    /// nobody remembering to check it -- which stays invisible until a peer sends
    /// one that is not text, by which time the bytes are in the leader's view of
    /// the fleet and in everything rendered from it.
    ///
    /// The extent is DEDUCED rather than spelled out. A row count written beside the
    /// rows is a second place the same fact lives, and the two part company the first
    /// time somebody appends one -- which is not hypothetical here: this table has
    /// already grown its fourth row once.
    constexpr std::array RegistrationTextFields {
        TextField<WorkerRegistration> { .name = "fingerprint",
                                        .project = [](WorkerRegistration const& r) { return r.fingerprint; } },
        TextField<WorkerRegistration> { .name = "endpoint",
                                        .project = [](WorkerRegistration const& r) { return r.endpoint; } },
        TextField<WorkerRegistration> { .name = "version",
                                        .project = [](WorkerRegistration const& r) { return r.version; } },
        // The fourth string this table's own comment anticipated. It matters more than
        // most: it is raw compiler output rather than anything this project composed,
        // so it is the likeliest field to arrive as bytes that are not text.
        TextField<WorkerRegistration> { .name = "toolchain label",
                                        .project = [](WorkerRegistration const& r) { return r.toolchainLabel; } },
    };

    /// Whether a registration's endpoint names the host it arrived from.
    ///
    /// **Hosts only, never ports**, which is forced rather than chosen: a peer dials
    /// from an ephemeral source port, so `peerId` carries none and there is nothing
    /// for an endpoint's port to be compared against. `Core/HostPort` owns the rest
    /// of the rule -- what an endpoint's host is, and when two hosts are the same
    /// machine.
    /// @param endpoint What the registration advertises, `host:port` or a bare host.
    /// @param peerHost The host the registration arrived from.
    /// @return True when the endpoint's host is the caller's own.
    [[nodiscard]] bool EndpointNamesCaller(std::string_view endpoint, std::string_view peerHost) noexcept
    {
        return SameHost(peerHost, HostOfEndpoint(endpoint));
    }

    /// Mismatch lines one leader will write before it goes quiet.
    ///
    /// The line carries the pair of addresses the counter cannot, and it is needed
    /// once per worker rather than once per registration: `Register` runs per
    /// toolchain and again on every re-registration, which a fleet does whenever a
    /// heartbeat is refused -- so an unhealthy fleet drives this hardest at exactly
    /// the moment its log is least readable. A cap keeps the diagnostic and drops the
    /// flood; the counter is what carries the rate afterwards, forever.
    ///
    /// Rate-limited rather than deduplicated because a table keyed on what a peer
    /// sent is a table a peer can grow, which this project already refuses for the
    /// discovery beacon's own provokable line.
    constexpr std::uint64_t MismatchLineBudget = 20;

    /// The counter a refusal moves, if any.
    /// @param code The refusal.
    /// @return Its counter, or nullopt when the code moves none.
    [[nodiscard]] constexpr std::optional<IMetricsSink::Counter> CounterFor(Wire::ErrorCode code) noexcept
    {
        // A range-based scan rather than `std::ranges::find`, and the reason is
        // portability rather than taste: over a `std::array`, libc++ and libstdc++
        // yield a raw pointer -- so clang-tidy's `readability-qualified-auto`
        // requires `auto const* const` -- while MSVC yields a class-type iterator
        // that such a declaration cannot deduce. There is no spelling of the
        // iterator that satisfies both. `CompileCacheWire::FindOp` already scans its
        // own table this way, so this is the tree's existing idiom as well as the
        // one that compiles everywhere.
        for (auto const& row: RefusalTable)
            if (row.code == code)
                return row.counter;
        return std::nullopt;
    }
} // namespace

SchedulerService::SchedulerService(IClock& clock,
                                   IWallClock const& wallClock,
                                   IMetricsSink& metrics,
                                   ILogger& logger,
                                   std::span<std::byte const> signingKey):
    _wallClock { wallClock },
    _metrics { metrics },
    _logger { logger },
    _signingKey { signingKey.begin(), signingKey.end() },
    _workers { clock },
    _leases { clock }
{
}

std::string SchedulerService::MintGrantToken(Distributed::Lease const& lease,
                                             std::string_view endpoint,
                                             std::string_view fingerprint)
{
    if (_signingKey.empty())
    {
        // Stated once, loudly, rather than left to look like it worked. Every other
        // way of getting here is worse: refusing to schedule would break every
        // single-machine install that has no `--cluster-key-file`, and saying nothing
        // is the failure mode this repository keeps rediscovering -- a fleet that is
        // green and is not doing the thing it claims.
        if (!_warnedUnsigned.exchange(true, std::memory_order_relaxed))
            _logger.Logf(LogLevel::Warn,
                         "handing out UNSIGNED lease grants: no cluster key is configured, so any client that can "
                         "reach a worker's compile port can spend it. Set --cluster-key-file on every node to close "
                         "this");
        return lease.token;
    }

    // The expiry is derived from the lease table's own lifetime rather than written
    // beside it. A token that outlived its lease would be a capability with no record
    // anywhere; one that died first would have a worker refusing work whose key this
    // scheduler is still suppressing.
    return MintLeaseToken(_signingKey,
                          LeaseClaims { .clusterId = ClusterId(),
                                        .epoch = _epoch.load(std::memory_order_relaxed),
                                        .serial = lease.token,
                                        .endpoint = std::string { endpoint },
                                        .fingerprint = std::string { fingerprint },
                                        .key = lease.key,
                                        .expiresAt = _wallClock.Now() + _leases.Timeout() });
}

void SchedulerService::SetRole(SchedulerRole role, std::string_view leaderEndpoint, std::uint64_t epoch)
{
    {
        std::scoped_lock const guard { _leaderMutex };
        _leaderEndpoint.assign(leaderEndpoint);
    }

    // Raised, never set. Terms only increase, so a lower one is either a stale
    // callback arriving out of order or a caller that does not know the term -- and
    // both are answered by keeping what we have. Obeying a decrease would let this
    // scheduler mint grants under an epoch its own workers have already refused past,
    // which is indistinguishable from the attack the epoch exists to stop.
    auto known = _epoch.load(std::memory_order_relaxed);
    while (epoch > known && !_epoch.compare_exchange_weak(known, epoch, std::memory_order_relaxed))
    {
    }

    ReconcileClusterId(role);

    // Published after the endpoint it describes, so a reader that sees `Leader`
    // has already been able to see the address that came with it.
    _role.store(role, std::memory_order_release);
}

void SchedulerService::ReconcileClusterId(SchedulerRole role)
{
    // Only where there is a cluster. Without consensus this node's own persisted
    // identity is the fleet's identity, and there is nothing to reconcile it with.
    if (_admin == nullptr)
        return;

    // The REPLICATED value wins, because every member must sign under one identity:
    // a cluster whose members each signed their own would refuse its own work at
    // every leadership change, which is the failure this whole field exists to
    // report and would be far worse than the bug it closes.
    if (auto const agreed = _admin->ClusterState().SettingOf(std::string { Cluster::ClusterIdSetting });
        agreed.has_value() && !agreed->empty())
    {
        SetClusterId(*agreed);
        return;
    }

    // Nothing agreed yet, so the first leader offers its own. A follower proposes
    // nothing -- `ProposeToCluster` would refuse it anyway, and asking is how a
    // refusal becomes a log line nobody can act on.
    if (role != SchedulerRole::Leader)
        return;

    auto const mine = ClusterId();
    if (mine.empty())
        return;

    // Once per process. A proposal is appended, not committed, so the state will not
    // show it immediately and an unguarded call would re-propose on every role change
    // until it lands.
    if (_proposedClusterId.exchange(true, std::memory_order_relaxed))
        return;

    if (auto const offered = _admin->ProposeToCluster(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                                                        .key = std::string { Cluster::ClusterIdSetting },
                                                                        .value = mine,
                                                                        .schedulerEndpoint = {} });
        !offered.has_value())
        // Not fatal: this node keeps signing under its own identity, which is right
        // for a single-member cluster and is what a worker has pinned anyway. Said
        // out loud because a fleet whose members never agree on an identity refuses
        // work after every election, and this line is the only warning of it.
        _logger.Logf(LogLevel::Warn,
                     "cluster: could not publish this fleet's identity ({}); members may not agree on it",
                     offered.error().context);
}

void SchedulerService::SetClusterId(std::string_view clusterId)
{
    std::scoped_lock const guard { _leaderMutex };
    _clusterId.assign(clusterId);
}

std::string SchedulerService::ClusterId() const
{
    std::scoped_lock const guard { _leaderMutex };
    return _clusterId;
}

SchedulerReply SchedulerService::Offer(Cluster::Command const& command)
{
    // Asked here as well as by whoever proposes, and `Validate` in full rather than
    // the one rule this surface happens to care about.
    //
    // The honest statement of why: `IClusterAdmin` is an untrusted seam. Its contract
    // says nothing about validation, so an implementation that skipped it would take
    // an empty key, an endpointless member and an unknown setting name as readily as
    // a name that is not text -- and this surface is what answers an operator. With
    // the only implementation in this tree, `ConsensusTier`, the reply is identical
    // either way, so what this buys is that the answer does not depend on which
    // implementation is behind the seam.
    //
    // Safe to refuse here precisely because this door is one-shot: an operator typed
    // `--cluster-admit`, `--cluster-set` or `--cluster-forget` and is reading the
    // answer. The reconciler is where the identical refusal is a trap, because it
    // would re-offer the same command every interval forever.
    if (auto const allowed = Cluster::Validate(command); !allowed.has_value())
        return Refuse(WireCodeFor(allowed.error().code), allowed.error().context);

    auto const proposed = _admin->ProposeToCluster(command);
    if (proposed.has_value())
        // Appended, not committed, and the reply says only that. A leader cannot
        // know the difference until a majority answers, and an operator who wants
        // to see the result asks for the state again -- which is a round trip they
        // were going to make anyway.
        return SchedulerReply::Success();

    // The context rather than the code as the message, because these refusals are
    // read by a person: "no such cluster setting: upsteam" is actionable and a
    // numeric code is not. `NotLeader` is the exception the other way -- its
    // message is a machine-readable endpoint a client redirects to -- so it names
    // the leader when consensus knew one.
    auto const& error = proposed.error();
    auto const code = WireCodeFor(error.code);
    if (code == Wire::ErrorCode::NotLeader)
        return Refuse(code, error.knownLeader.value_or(std::string {}));
    return Refuse(code, error.context);
}

SchedulerReply SchedulerService::ClusterStatus(CallerContext const& caller)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return SchedulerReply::Success(Cluster::Encode(_admin->ClusterState()));
}

SchedulerReply SchedulerService::ClusterSet(CallerContext const& caller, std::string_view name, std::string_view value)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return Offer(Cluster::Command { .kind = Cluster::CommandKind::SetSetting,
                                    .key = std::string { name },
                                    .value = std::string { value },
                                    .schedulerEndpoint = {} });
}

SchedulerReply SchedulerService::ClusterForget(CallerContext const& caller, std::string_view memberId)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    return Offer(Cluster::Command {
        .kind = Cluster::CommandKind::RemoveMember, .key = std::string { memberId }, .value = {}, .schedulerEndpoint = {} });
}

SchedulerReply SchedulerService::ClusterAdmit(CallerContext const& caller,
                                              std::string_view memberId,
                                              std::string_view raftEndpoint)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);
    if (_admin == nullptr)
        return Refuse(Wire::ErrorCode::NoCluster);

    // `schedulerEndpoint` left empty, which `AddMember` applies wholesale -- so
    // re-admitting a member that has moved clears whatever it had announced, and it
    // announces the new one on its next election. That is the right way round: a
    // node that moved has moved both ports, and keeping the old scheduler endpoint
    // would redirect clients to an address that member no longer answers.
    return Offer(Cluster::Command { .kind = Cluster::CommandKind::AddMember,
                                    .key = std::string { memberId },
                                    .value = std::string { raftEndpoint },
                                    .schedulerEndpoint = {} });
}

SchedulerReply SchedulerService::Refuse(Wire::ErrorCode code, std::string message) const
{
    if (auto const counter = CounterFor(code); counter.has_value())
        _metrics.Increment(*counter);
    return SchedulerReply { .status = Wire::Status::Error, .error = code, .message = std::move(message), .payload = {} };
}

std::optional<SchedulerReply> SchedulerService::Gate(CallerContext const& caller) const
{
    // Leadership first, and not only because it is cheaper to answer. A follower
    // holds a registry that is a stale copy of somebody else's, so admitting a
    // worker here would put it in a fleet nothing schedules onto -- and the worker
    // would heartbeat happily into it forever. Refusing with the leader's address
    // is what turns that into one redirect.
    if (Role() != SchedulerRole::Leader)
        return Refuse(Wire::ErrorCode::NotLeader, LeaderEndpoint());

    // Then the anti-leeching rule, which is the half the transport can also ask
    // before it reads a payload -- so it lives in one function that both call.
    return RefuseUnlessMember(caller);
}

std::optional<SchedulerReply> SchedulerService::RefuseUnlessMember(CallerContext const& caller) const
{
    // A non-member is *not* refused the cache -- it reads and writes objects exactly
    // as before -- it is refused the fleet's CPU time, which is the thing membership
    // pays for.
    if (caller.membership != Membership::Member)
        return Refuse(Wire::ErrorCode::NotAMember);

    return std::nullopt;
}

SchedulerReply SchedulerService::Register(CallerContext const& caller, WorkerRegistration const& registration)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // Checked HERE, where the wire becomes fleet state, rather than in whichever
    // renderer notices first. Everything below copies these strings into the
    // leader's view of the fleet, and `/fleet.json`, `/fleet`, `--cluster-status`
    // and the logs all read them back out again -- so a renderer that repaired
    // them would be a second place the value is decided, and the surfaces that did
    // not repair would still carry the originals.
    //
    // The whole registration goes, not the offending field: the fingerprint is
    // matched byte for byte, so a worker admitted with a blanked-out one would
    // match nothing and sit in the fleet never being picked. A refusal reaches the
    // worker's own log through `DescribeOutcome`; the counter is what an operator
    // sees when the peer is not one of ours and never says anything at all.
    if (auto const field = FirstFieldNotText(registration, RegistrationTextFields); field.has_value())
        return Refuse(Wire::ErrorCode::MalformedRegistration, NotTextRefusal(*field));

    // No zero-slot refusal any more, and its removal is a decision rather than a
    // simplification. A zero used to mean "a worker that will never be picked" and
    // was refused for that reason; since `OfferableSlots` it means "size me from my
    // own hardware", which is the *preferred* spelling -- a node that computed its
    // own slot count would be the one place a workstation's reserve could be got
    // wrong with nothing downstream able to tell. The failure the refusal protected
    // against is closed by construction instead: `OfferableSlots` never returns
    // zero, so no registration can produce a worker that matches leases and is
    // never picked.

    // Measured, deliberately NOT refused (#242).
    //
    // A registration asserts where work for a toolchain should be sent and nothing
    // ties that claim to the connection carrying it. Refusing a mismatch was priced
    // and REJECTED -- it turns away the documented setup, and stops only a third
    // host, since membership already admitted this one. The full argument, and what
    // closing it properly needs, is the #242 entry in
    // `.agent/rules/distributed-compilation.md`; do not re-derive it from here.
    //
    // So: a counter, a bounded line, and no wire code at all -- one that nothing
    // returns would put a lie in the refusal table.
    if (!EndpointNamesCaller(registration.endpoint, caller.peerId))
    {
        _metrics.Increment(IMetricsSink::Counter::DispatchWorkerEndpointMismatch);

        // `Info`, not a warning. On a fleet that advertises DNS names this is every
        // registration and nothing is wrong, and a signal that fires permanently on
        // correct deployments is one operators learn to filter -- so it would not be
        // there on the day it means something. Info is the default production level,
        // so it is still read.
        if (auto const written = _mismatchLines.fetch_add(1, std::memory_order_relaxed); written < MismatchLineBudget)
            _logger.Logf(LogLevel::Info,
                         "dispatch: worker at {} registered endpoint {} for {}, which this scheduler does not verify "
                         "(#242); expected with DNS names, NAT, VPN or multi-homing{}",
                         caller.peerId.empty() ? std::string_view { "an unnameable peer" } : caller.peerId,
                         registration.endpoint,
                         registration.fingerprint,
                         written + 1 == MismatchLineBudget ? " -- further mismatches are counted only" : "");
    }

    auto const id = _workers.Register(registration);

    // A re-registration deliberately does NOT release this worker's leases, even
    // though it resets `inFlight` two lines above on the reasoning that whatever it
    // was running is gone. The two are not the same bet: a node re-registers after
    // any refused heartbeat -- `EndpointBusy`, or `NotLeader` during an election --
    // and not only after a restart, so releasing here would wipe leases for compiles
    // that are still running and hand a second client the same work. `inFlight`
    // recovers from that guess at the next heartbeat; a lease does not.
    //
    // A worker that genuinely restarted needs nothing here anyway: its client's
    // compile exchange fails, and the client resolves its own lease on that path
    // like every other (#212).

    // Counted as an event, not as fleet size. This interface is counter-only, so it
    // cannot express a gauge -- and the event turns out to be the more useful
    // number anyway: a rate that stays high means workers keep re-registering,
    // which is what a fleet whose heartbeats are not arriving looks like from the
    // scheduler's side.
    _metrics.Increment(IMetricsSink::Counter::DispatchWorkerRegistrations);

    auto const bytes = Wire::AsBytes(id);
    return SchedulerReply::Success(std::vector<std::byte> { bytes.begin(), bytes.end() });
}

void SchedulerService::SetHistorySink(IFleetHistorySink* sink) noexcept
{
    _history = sink;
}

SchedulerReply SchedulerService::Heartbeat(CallerContext const& caller,
                                           std::string_view workerId,
                                           NodeLoad const& load,
                                           std::span<FleetBucket const> history)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // An unknown id is answered, not ignored: it means the scheduler restarted or
    // expired this worker, and the worker's correct response is to register again.
    // Silence would leave it heartbeating into a void forever while the fleet ran
    // without it.
    auto const endpoint = _workers.Heartbeat(workerId, load);
    if (!endpoint.has_value())
        return Refuse(Wire::ErrorCode::UnknownLease, "unknown worker; register again");

    // Under the ENDPOINT, never the worker id: a machine with two `--toolchain`
    // flags heartbeats twice with the same figures, and keying per id would have the
    // fleet counting one machine's contribution once per toolchain -- the rule
    // `WorkerRegistry::NodeCaches()` already exists to enforce on the neighbouring
    // number.
    if (_history != nullptr && !history.empty())
        _history->AcceptHistory(*endpoint, history);

    return SchedulerReply::Success();
}

void SchedulerService::ReapExpiredWorkers()
{
    // Two locks, taken one after the other rather than one spanning both, and the
    // gap is deliberate: the registry and the lease table each guard their own, and
    // a lock covering the pair would put every reader of either behind the other.
    // What fits in the gap is a concurrent `Lease` that picked this worker just
    // before it was erased, whose lease is then held against a worker no later reap
    // can name. It is not stranded: the client that took it resolves it when its
    // job ends, and expiry is behind that -- which is exactly the pair of
    // guarantees every lease already has.
    auto const dropped = _workers.ExpireStale();
    if (dropped.empty())
        // The ordinary case on a live fleet, and this is a hot path -- every lease
        // request runs it.
        return;

    std::size_t reclaimed = 0;
    for (auto const& workerId: dropped)
        reclaimed += _leases.ReleaseWorker(workerId);

    _metrics.Increment(IMetricsSink::Counter::DispatchWorkersExpired, static_cast<std::uint64_t>(dropped.size()));
    if (reclaimed != 0)
        _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReclaimed, static_cast<std::uint64_t>(reclaimed));
}

SchedulerReply SchedulerService::Lease(CallerContext const& caller, Wire::LeaseRequest const& request)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // Before the key is asked about, not after: a worker that vanished mid-job left
    // its leases behind, and duplicate suppression consults the key first -- so
    // every client that later missed on one of those keys was refused
    // `AlreadyInFlight` and compiled locally until the lease timed out. Losing one
    // machine quietly stopped distributing part of the build.
    ReapExpiredWorkers();

    // Duplicate suppression is asked BEFORE capacity, and the order is the
    // diagnostic. `Acquire` needs a worker id, so the code this was lifted from had
    // to pick first -- which meant a second client missing the same key at a busy
    // fleet was told `NoCapacity`. That reads as "buy more machines" when the truth
    // is "this build asked for the same object twice", and it lands hardest exactly
    // where duplicate suppression does the most good: a wide parallel build where
    // many translation units miss one key at once. Same refusal either way, so no
    // client behaviour changes; only what an operator is told about their fleet.
    if (_leases.IsInFlight(request.key))
        // Not a failure: duplicate-work suppression refusing the second of many
        // clients that missed the same key, each of which compiles locally.
        return Refuse(Wire::ErrorCode::AlreadyInFlight);

    auto const picked = _workers.Pick(request.fingerprint);
    if (!picked.has_value())
        // Counted apart by the table above, because they are three different
        // operator problems: no worker means the fleet is misconfigured (a
        // fingerprint nobody serves), no capacity means it is too small, and
        // withdrawn means it is big enough and its machines are doing something
        // else. Summing any two of them hides the more actionable one.
        //
        // A table rather than a `switch`, so a fourth `PickError` is a build failure
        // here rather than a refusal that silently arrives as one of the other
        // three.
        return Refuse(WireCodeFor(picked.error()));

    auto const lease = _leases.Acquire(request.key, picked->id);
    if (!lease.has_value())
        // Still reachable, and the reason it must stay: `IsInFlight` above is
        // advisory, so two callers can both pass it and race here. `Acquire` is the
        // one that decides atomically, and the loser gets the same refusal it would
        // have got a few microseconds earlier.
        return Refuse(Wire::ErrorCode::AlreadyInFlight);

    // Accounted only once the lease exists. Counting at Pick would inflate the load
    // of a worker whose key turned out to be already in flight, and the correction
    // would not arrive until its next heartbeat.
    _workers.JobStarted(picked->id);
    _metrics.Increment(IMetricsSink::Counter::DispatchLeasesGranted);

    // Signed over the endpoint as well as the key, which is what makes it a grant on
    // ONE worker rather than on the fleet: without the endpoint inside the MAC, a
    // lease minted for this machine replays against every other machine that trusts
    // the same key.
    //
    // The PICKED worker's fingerprint, not the request's, though `Pick` matches the
    // two byte for byte and they are equal today. What the worker will compare the
    // claim against is its own registered fingerprint, so taking it from the same
    // record the endpoint comes from keeps both halves of the binding local to this
    // registry entry -- rather than resting on `Pick`'s comparison staying exact.
    auto const token = MintGrantToken(*lease, picked->endpoint, picked->fingerprint);

    // The worker's codecs travel with the grant so the client can choose one for the
    // preprocessed payload it is about to send -- without a negotiation round trip,
    // and without guessing at something the worker cannot decode after the whole
    // payload has already crossed the network.
    return SchedulerReply::Success(Wire::EncodeLeaseGrant(
        Wire::LeaseGrant { .endpoint = picked->endpoint, .leaseToken = token, .workerCodecs = picked->codecs }));
}

SchedulerReply SchedulerService::Release(CallerContext const& caller, std::string_view leaseToken, std::string_view key)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // The token the client hands back is the SIGNED grant, so the serial the lease
    // table knows has to be unwrapped out of it -- which is also the point at which a
    // release that was never granted stops being able to resolve anything. Verified
    // rather than merely parsed: it costs one HMAC on a verb that already crossed the
    // network, and a forged release frees a key somebody else is building.
    //
    // Only when this scheduler signs. Without a key it never wrapped anything, so the
    // token is the serial, and demanding otherwise would refuse every lease it had
    // itself just issued.
    std::string serial { leaseToken };
    if (!_signingKey.empty())
    {
        auto authentic = AuthenticateLeaseToken(_signingKey, leaseToken);
        if (!authentic.has_value())
            return Refuse(Wire::ErrorCode::LeaseUnauthorized);
        serial = std::move(authentic->serial);
    }

    auto const lease = _leases.Release(serial, key);
    if (!lease.has_value())
        // Already gone: it expired under a job that outlived its lease, this is a
        // second release of one token, or the token belongs to a scheduler instance
        // that no longer exists and this one has since reissued the number.
        // Answered rather than accepted silently, because the first of those is a
        // fleet whose `DefaultLeaseTimeout` is shorter than its slowest translation
        // unit -- and a client that is told nothing has nothing to report.
        // Uncounted by design: it is a statement about one client's timing, not
        // about the fleet's capacity.
        return Refuse(Wire::ErrorCode::UnknownLease, "unknown or already-resolved lease");

    // The registry's speculative count, undone. `JobStarted` at `Lease` above is what
    // stops the scheduler over-assigning a worker inside one heartbeat window, and
    // without this it only ever climbed -- corrected solely by the next heartbeat
    // overwrite, so a burst of leases took a worker out of rotation until it landed.
    _workers.JobFinished(lease->workerId);
    _metrics.Increment(IMetricsSink::Counter::DispatchLeasesReleased);
    return SchedulerReply::Success();
}

} // namespace FastCache::Distributed
