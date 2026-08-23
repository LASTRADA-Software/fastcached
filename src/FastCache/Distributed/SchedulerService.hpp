// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Clock.hpp>
#include <FastCache/Distributed/LeaseTable.hpp>
#include <FastCache/Distributed/WorkerRegistry.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
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
enum class SchedulerRole : std::uint8_t
{
    /// Not the leader. Refuse and redirect if a leader is known.
    Follower = 0,
    /// No leader is known -- an election is in progress.
    Undecided,
    /// This node leads and may hand out capacity.
    Leader,
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
    /// Who the peer says it is, for logs. Never trusted for a decision.
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
    /// @param metrics Counts the outcomes below; must outlive the service.
    SchedulerService(IClock& clock, IMetricsSink& metrics) noexcept;

    /// Publish this node's current standing in the cluster.
    ///
    /// Called by the consensus driver whenever leadership moves. Kept as a setter
    /// rather than a constructor parameter because leadership is exactly the thing
    /// that changes while the object lives -- the documented carve-out to
    /// configuration-at-construction, not an exception to it.
    /// @param role What this node may do.
    /// @param leaderEndpoint Where the leader is, when one is known; empty otherwise.
    void SetRole(SchedulerRole role, std::string_view leaderEndpoint);

    /// This node's current standing.
    /// @return The role last published.
    [[nodiscard]] SchedulerRole Role() const noexcept
    {
        return _role;
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
    [[nodiscard]] SchedulerReply Heartbeat(CallerContext const& caller, std::string_view workerId, NodeLoad const& load);

    /// Pick a worker and authorize one job on it.
    /// @param caller Who is asking.
    /// @param request The toolchain, the object key and the client's codecs.
    /// @return `Ok` carrying an encoded `LeaseGrant`, or a refusal.
    [[nodiscard]] SchedulerReply Lease(CallerContext const& caller, CompileCacheWire::LeaseRequest const& request);

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
    /// The two gates every verb passes, in the order that costs least to answer.
    ///
    /// One function rather than a pair of checks repeated three times: these are the
    /// security- and policy-relevant decisions of the whole surface, and a verb
    /// added without them would otherwise be a verb that quietly skips both.
    /// @param caller Who is asking.
    /// @return A refusal, or nullopt when the caller may proceed.
    [[nodiscard]] std::optional<SchedulerReply> Gate(CallerContext const& caller) const;

    /// Build a refusal, counting it when the table names a counter.
    /// @param code Why.
    /// @param message Overrides the table default when non-empty.
    /// @return The refusal.
    [[nodiscard]] SchedulerReply Refuse(CompileCacheWire::ErrorCode code, std::string message = {}) const;

    IMetricsSink& _metrics;
    WorkerRegistry _workers;
    LeaseTable _leases;
    SchedulerRole _role { SchedulerRole::Undecided };
    std::string _leaderEndpoint {};
};

} // namespace FastCache::Distributed
