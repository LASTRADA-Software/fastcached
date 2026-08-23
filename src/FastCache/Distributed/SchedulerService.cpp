// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/SchedulerService.hpp>

#include <algorithm>
#include <array>
#include <optional>
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
        RefusalDescriptor { .code = Wire::ErrorCode::AlreadyInFlight,
                            .counter = IMetricsSink::Counter::DispatchLeasesDuplicate },
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
                                             Wire::ErrorCode::UnknownLease };

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

SchedulerService::SchedulerService(IClock& clock, IMetricsSink& metrics) noexcept:
    _metrics { metrics },
    _workers { clock },
    _leases { clock }
{
}

void SchedulerService::SetRole(SchedulerRole role, std::string_view leaderEndpoint)
{
    _role = role;
    _leaderEndpoint.assign(leaderEndpoint);
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
    if (_role != SchedulerRole::Leader)
        return Refuse(Wire::ErrorCode::NotLeader, _leaderEndpoint);

    // Then the anti-leeching rule. A non-member is *not* refused the cache -- it
    // reads and writes objects exactly as before -- it is refused the fleet's CPU
    // time, which is the thing membership pays for.
    if (caller.membership != Membership::Member)
        return Refuse(Wire::ErrorCode::NotAMember);

    return std::nullopt;
}

SchedulerReply SchedulerService::Register(CallerContext const& caller, WorkerRegistration const& registration)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // A worker with no slots would register, match every lease for its toolchain,
    // and never be picked -- indistinguishable at the client from a fleet that is
    // permanently busy. Refuse it where it can be explained.
    if (registration.slots == 0)
        return Refuse(Wire::ErrorCode::MalformedFrame, "a worker must offer at least one slot");

    auto const id = _workers.Register(registration);
    // Counted as an event, not as fleet size. This interface is counter-only, so it
    // cannot express a gauge -- and the event turns out to be the more useful
    // number anyway: a rate that stays high means workers keep re-registering,
    // which is what a fleet whose heartbeats are not arriving looks like from the
    // scheduler's side.
    _metrics.Increment(IMetricsSink::Counter::DispatchWorkerRegistrations);

    auto const bytes = Wire::AsBytes(id);
    return SchedulerReply::Success(std::vector<std::byte> { bytes.begin(), bytes.end() });
}

SchedulerReply SchedulerService::Heartbeat(CallerContext const& caller, std::string_view workerId, std::uint32_t inFlight)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

    // An unknown id is answered, not ignored: it means the scheduler restarted or
    // expired this worker, and the worker's correct response is to register again.
    // Silence would leave it heartbeating into a void forever while the fleet ran
    // without it.
    if (!_workers.Heartbeat(workerId, inFlight))
        return Refuse(Wire::ErrorCode::UnknownLease, "unknown worker; register again");

    return SchedulerReply::Success();
}

SchedulerReply SchedulerService::Lease(CallerContext const& caller, Wire::LeaseRequest const& request)
{
    if (auto refusal = Gate(caller); refusal.has_value())
        return std::move(*refusal);

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
        // Counted apart by the table above, because they are different operator
        // problems: no worker means the fleet is misconfigured (a fingerprint
        // nobody serves), no capacity means it is too small. Summing them would
        // hide the first behind the second exactly when a fleet is busy.
        return Refuse(picked.error() == PickError::NoWorker ? Wire::ErrorCode::NoWorker : Wire::ErrorCode::NoCapacity);

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

    // The worker's codecs travel with the grant so the client can choose one for the
    // preprocessed payload it is about to send -- without a negotiation round trip,
    // and without guessing at something the worker cannot decode after the whole
    // payload has already crossed the network.
    return SchedulerReply::Success(Wire::EncodeLeaseGrant(
        Wire::LeaseGrant { .endpoint = picked->endpoint, .leaseToken = lease->token, .workerCodecs = picked->codecs }));
}

} // namespace FastCache::Distributed
