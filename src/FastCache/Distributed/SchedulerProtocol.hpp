// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Distributed/FleetHistory.hpp>
#include <FastCache/Distributed/SchedulerService.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace FastCache::Distributed
{

/// Render a node's capacity in the vocabulary the wire uses.
///
/// The counterpart of `CapacityFromWire`, and they are declared together on
/// purpose: they are one mapping written twice, and the failure they invite is a
/// transposition — cores read as memory, a reserve read as a class. Kept side by
/// side so a round trip can pin them against each other, which is what
/// `SchedulerProtocol_test` does with every field set to a distinct value.
///
/// This direction cannot fail. `NodeClass` is an enumerator, so it always has a
/// byte; the reverse is not true, which is why only that one returns an optional.
/// @param capacity What this machine is.
/// @return The same facts, as raw wire values.
[[nodiscard]] CompileCacheWire::CapacityFields CapacityToWire(NodeCapacity const& capacity);

/// Read a capacity record back into the scheduler's vocabulary.
///
/// Fails only on a node class this build does not know — see `NodeClassFromRaw`
/// for why that is a refusal rather than a guess.
/// @param fields The raw values as received.
/// @return The facts, or nullopt when the class byte is not one of ours.
[[nodiscard]] std::optional<NodeCapacity> CapacityFromWire(CompileCacheWire::CapacityFields const& fields);

/// Render a node's cache budget in the vocabulary the wire uses.
///
/// Paired with `CacheCapacityFromWire` for the reason every other pair here is,
/// and with one extra hazard of its own: the wire carries tiers **positionally**,
/// because `CompileCacheWire.hpp` is compiled into `fastcache-cc` and cannot see
/// `StorageTier`. So these two functions are the only place that knows position
/// *k* means the *k*-th enumerator, and a transposition between them is invisible
/// to every compiler and to both ends of the wire — which is why
/// `SchedulerProtocol_test` round-trips them with a distinct value per tier.
/// @param cache What the node's cache is configured to hold.
/// @return The same facts, as raw wire values.
[[nodiscard]] CompileCacheWire::CacheCapacityFields CacheCapacityToWire(NodeCacheCapacity const& cache);

/// Read a cache-budget record back into the scheduler's vocabulary.
///
/// Cannot fail. A tier position this build does not know comes from a peer that
/// knows one it does not, and is dropped rather than refused: unlike a node
/// class, a cache figure is reported and never acted on, so there is no decision
/// to get silently wrong.
/// @param fields The raw values as received.
/// @return The facts.
[[nodiscard]] NodeCacheCapacity CacheCapacityFromWire(CompileCacheWire::CacheCapacityFields const& fields);

/// Render what a node's cache holds in the vocabulary the wire uses.
/// @param cache What the cache holds right now.
/// @return The same facts, as raw wire values.
[[nodiscard]] CompileCacheWire::CacheLoadFields CacheLoadToWire(NodeCacheLoad const& cache);

/// Read a cache-usage record back into the scheduler's vocabulary.
/// @param fields The raw values as received.
/// @return The facts.
[[nodiscard]] NodeCacheLoad CacheLoadFromWire(CompileCacheWire::CacheLoadFields const& fields);

/// Render a node's live load in the vocabulary the wire uses.
///
/// Paired with `LoadFromWire` for the reason `CapacityToWire` is paired with
/// `CapacityFromWire`: one mapping written twice, whose characteristic failure is a
/// transposition that no compiler can see. `inFlight` is deliberately not part of
/// the pair — it travels as a field of HEARTBEAT itself rather than inside the
/// nested record, because it is the one number a worker can never fail to have.
/// @param load What this machine is doing.
/// @return The same facts, as raw wire values.
[[nodiscard]] CompileCacheWire::LoadFields LoadToWire(NodeLoad const& load);

/// Read a live-load record back into the scheduler's vocabulary.
/// @param fields The raw values as received.
/// @param inFlight The job count, which travels outside the record.
/// @return The facts.
[[nodiscard]] NodeLoad LoadFromWire(CompileCacheWire::LoadFields const& fields, std::uint32_t inFlight);

/// The wire carries exactly `FleetMetric`'s slots, positionally.
///
/// `CompileCacheWire.hpp` cannot include `FleetHistory.hpp` -- it stays
/// dependency-free because `fastcache-cc` compiles it without linking `FastCache` --
/// so the count is spelled there too, and this file, which converts between the two,
/// is the one place both spellings are visible. A tenth metric added without moving
/// `HistorySlotCount` would have every peer reading nine of them and silently
/// dropping the tenth, which is the same shape of defect `StorageTier`'s positional
/// carrying already records.
static_assert(EnumeratorCount<FleetMetric> == CompileCacheWire::HistorySlotCount,
              "a slot added to FleetMetric must move HistorySlotCount, or it never reaches the leader");

/// Convert closed buckets to the shape the wire carries.
/// @param buckets What a node is handing over.
/// @return The wire records, in the same order.
[[nodiscard]] std::vector<CompileCacheWire::HistoryBucketFields> HistoryToWire(std::span<FleetBucket const> buckets);

/// Convert wire records back to buckets.
///
/// `present` is set on every one of them, because a node only ever hands over
/// buckets it recorded -- there is nothing on the wire that could mean a gap, and a
/// gap is the absence of a record rather than a record saying so.
/// @param records What arrived.
/// @return The buckets, in the same order.
[[nodiscard]] std::vector<FleetBucket> HistoryFromWire(std::span<CompileCacheWire::HistoryBucketFields const> records);

/// Turn one `0xFC` request into one reply, for a fleet scheduler.
///
/// **Pure**: bytes in, bytes out, exactly as `Cc::WorkerProtocol` is on the other
/// side of the same wire. It opens no socket, so the whole verb surface is tested
/// by handing it a frame and reading the answer -- with no listener, no reactor and
/// no cluster.
///
/// A scheduler answers the three dispatch verbs -- `Register`, `Heartbeat` and
/// `Lease` -- and the three that administer the cluster it leads. Everything else
/// -- `Store` and `Fetch` above all -- is refused with
/// `DispatchNotPermitted`, because a scheduler is not a cache. That refusal is a
/// *reply* and not a close: a client that sent a cache verb to the scheduler's port
/// learns which, rather than seeing a dropped connection it cannot tell from a dead
/// host. The same reasoning `WorkerProtocol` records for refusing the scheduler's
/// verbs, pointing the other way.
///
/// ## The verb set is a table, and it is the security-relevant one
///
/// `IsSchedulerVerb` is derived from `SchedulerOps` rather than from a `switch`, so
/// a verb reaching this class without a row is refused by construction. That is the
/// direction a mistake has to fail in here for the same reason it is in
/// `OpDescriptor::preAuth`: what an unauthenticated -- or, here, non-member --
/// peer can reach is a property a reviewer must be able to read off a table.
class SchedulerProtocol
{
  public:
    /// @param service Decides every request; must outlive this.
    /// @param service What decides every verb this surface routes.
    /// @param metrics Where this surface's own refusals are recorded.
    ///
    /// **Two arguments and no longer `explicit`-with-one**, because this class
    /// refuses frames the service never sees: a version it does not serve, an opcode
    /// with no row, a verb served on another port, a header that disagrees with its
    /// own payload. Those are decided here and were counted nowhere, on the one
    /// surface that carries lease grants and worker registration -- so an operator
    /// watching it being probed saw the same flat line as one nobody was talking to
    /// ([#494](https://github.com/LASTRADA-Software/fastcached/issues/494)).
    ///
    /// The sink is the one `SchedulerTier` already holds, so nothing new is
    /// constructed and no call site above it changes.
    SchedulerProtocol(SchedulerService& service, IMetricsSink& metrics) noexcept:
        _service { service },
        _metrics { metrics }
    {
    }

    /// Answer one complete request frame.
    ///
    /// Takes the frame whole rather than a header and a payload, because the caller
    /// has already had to read a declared length in order to know where the frame
    /// ended -- which is the property the `[magic][version][op][u32 length]` header
    /// exists to give, and the reason every refusal below can be a reply.
    /// @param frame Header plus payload, exactly as received.
    /// @param caller Who is asking, gathered by the transport.
    /// @return The encoded reply. Never empty: a refusal is still an answer.
    [[nodiscard]] std::vector<std::byte> Answer(std::span<std::byte const> frame, CallerContext const& caller);

    /// Refuse a caller the surface will not serve, without seeing its request.
    ///
    /// The encoded form of `SchedulerService::RefuseUnlessMember`, for a transport
    /// that wants the answer **before it buffers a payload** (#285). It exists here
    /// rather than in the transport because encoding a `SchedulerReply` into wire
    /// bytes is this class's job, and because the predicate must stay the service's
    /// -- the transport asks, it does not decide.
    /// @param caller Who is asking, gathered by the transport.
    /// @return The encoded refusal, or nullopt when the caller is admitted.
    [[nodiscard]] std::optional<std::vector<std::byte>> RefusePeer(CallerContext const& caller) const;

  private:
    /// Decode one verb's payload and hand it to the service.
    ///
    /// Named `Route` rather than `Dispatch` deliberately: `RedisResp.cpp` already
    /// defines a free `Dispatch()` in namespace `FastCache`, and that collision
    /// under unqualified lookup is why this whole module is spelled `Distributed`
    /// in the first place. Re-introducing the word here would put the trap back one
    /// scope down.
    /// @param op The verb, already known to be one of `SchedulerOps`.
    /// @param payload The request payload, its length already checked.
    /// @param caller Who is asking.
    /// @return What the service decided, or a malformed-frame refusal.
    [[nodiscard]] SchedulerReply Route(CompileCacheWire::Op op,
                                       std::span<std::byte const> payload,
                                       CallerContext const& caller);

    SchedulerService& _service;
    IMetricsSink& _metrics;
};

} // namespace FastCache::Distributed
