// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    // **Triaged in #494.** Four of this surface's seven arms count -- the ones the
    // service never sees, where a frame is refused before any verb reaches it. The
    // other three defer to `SchedulerService`, which triages its own refusals
    // completely (`RefusalTable`, `UncountedRefusals`, and `RefusalsAreDisjoint()`
    // proving every code is in exactly one). Each arm states its own reasoning.

    /// The verbs a scheduler answers.
    ///
    /// A table rather than three `case` labels so the membership test below is
    /// derived from it: a verb that reaches this class without a row is refused
    /// rather than served, which is the direction a mistake has to fail in.
    constexpr std::array SchedulerOps { Wire::Op::Register,      Wire::Op::Heartbeat,     Wire::Op::Lease,
                                        Wire::Op::Release,       Wire::Op::ClusterStatus, Wire::Op::ClusterSet,
                                        Wire::Op::ClusterForget, Wire::Op::ClusterAdmit };

    /// Whether this scheduler serves @p op at all.
    /// @param op The verb, already resolved against `OpTable`.
    /// @return True when it is one of `SchedulerOps`.
    [[nodiscard]] constexpr bool IsSchedulerVerb(Wire::Op op) noexcept
    {
        return std::ranges::find(SchedulerOps, op) != SchedulerOps.end();
    }

    /// This surface's rows. The shape, the lookup and why they exist are on
    /// `Wire::RefusedVerb`; what belongs here is only which verbs and what they say.
    ///
    /// **Empty since #289**, and the removal is the point. The row that was here said
    /// `Auth` was `UnimplementedVerb` -- "this endpoint schedules and checks no
    /// credential" -- which was true and is not any more: the scheduler surface
    /// terminates `AUTH` in `FrameServer`'s loop, because what that verb changes is
    /// per-connection state and this class is deliberately stateless.
    ///
    /// Leaving the row would have been the exact failure the rulebook records twice
    /// (#283, #340): `UnimplementedVerb` tells `Cc::CacheProtocol::Exchange` to step
    /// over the refusal and proceed UNAUTHENTICATED, so a launcher holding the right
    /// token would have skipped presenting it and then had every gated verb refused --
    /// a green build and a fleet distributing nothing, which is the shape the removed
    /// row was itself added to fix, pointing the other way.
    ///
    /// Kept as an empty table rather than deleted so the next verb this surface
    /// refuses is a row, not a `case`.
    constexpr std::array<Wire::RefusedVerb, 0> RefusedVerbs {};

    // The table is consulted only on the path a verb this scheduler does NOT serve
    // takes, so a row naming one it does serve is never read -- it would sit there
    // looking like a decision and change nothing. Refused at compile time rather
    // than left to be noticed, which is the same argument `RowsInEnumeratorOrder`
    // makes: a guard that can only fire when the table is wrong.
    static_assert(std::ranges::none_of(RefusedVerbs, IsSchedulerVerb, &Wire::RefusedVerb::op),
                  "a refusal row for a verb this scheduler serves is dead: the lookup never reaches it");
} // namespace

std::optional<std::vector<std::byte>> SchedulerProtocol::RefusePeer(CallerContext const& caller) const
{
    auto const refusal = _service.RefuseUnlessMember(caller);
    if (!refusal.has_value())
        return std::nullopt;
    // Encoded exactly as `Answer` encodes a refusal, so an early refusal and a late
    // one are the same bytes on the wire -- a client must not be able to tell which
    // side of the payload read it was refused on.
    // **Uncounted, and decided by `SchedulerService` rather than here.** This arm
    // hands back whatever `RefuseUnlessMember` produced -- today `NotAMember`, which
    // the service lists in `UncountedRefusals` on the argument that counting a policy
    // answer beside the capacity refusals would put noise into the numbers a fleet is
    // sized from.
    //
    // A counter here would also count only ONE of the two paths to that refusal: the
    // same condition is answered again by `Route` -> `Gate` after the payload is read,
    // and the two are deliberately byte-identical on the wire. Counting the early path
    // alone would under-report by an amount that varies with frame size, which is a
    // worse number than none.
    //
    // Whether the scheduler should count non-member callers in a series of its OWN --
    // the service's reason argues against mixing rather than against counting, and
    // this is the surface a credential-guessing client reaches first -- is
    // [#592](https://github.com/LASTRADA-Software/fastcached/issues/592), which
    // belongs in the service's table beside the existing decision.
    return Cc::RefuseWithoutCounter(
        { .code = refusal->error,
          .rationale = "SchedulerService::UncountedRefusals already decided this refusal, and both paths to it must "
                       "agree; counting only the pre-payload path would under-report. See #592" },
        refusal->message);
}

std::vector<std::byte> SchedulerProtocol::Answer(std::span<std::byte const> frame, CallerContext const& caller)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic or a short read. This is the one condition a caller cannot
        // answer *and* resynchronize from -- with no declared length there is no
        // way to find where the frame ended -- so the transport closes, and an
        // empty answer is how it is told to.
        return {};

    if (!Wire::IsSupported(header->version))
        // Naming the supported range, because a rejection that cannot say what
        // would have worked cannot be acted on.
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::UnsupportedVersion,
                            .counter = IMetricsSink::Counter::DispatchFramesRefusedUnsupportedVersion },
                          std::format("supported versions {}..{}",
                                      static_cast<unsigned>(Wire::MinSupportedVersion),
                                      static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        // Stepped over rather than fatal: the framing exists so a receiver can skip
        // a verb it does not know, which is what lets a newer client talk to an
        // older scheduler at all.
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::UnknownOpcode,
                            .counter = IMetricsSink::Counter::DispatchFramesRefusedUnknownOpcode });

    if (!IsSchedulerVerb(descriptor->code))
    {
        // **Uncounted, for two independent reasons, and both are recorded because only
        // one of them survives a row being added.**
        //
        // The code comes from the MATCHED ROW, so a counter beside it would have to be
        // built from a table that has no counter column -- the same argument
        // `CacheProxy`'s equivalent arm carries. And `RefusedVerbs` is empty here, so
        // this branch cannot fire at all today. The first reason still holds the day
        // somebody adds a row; the second stops holding at that moment.
        if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
            return Cc::RefuseWithoutCounter({ .code = row->code,
                                              .rationale = "the code comes from the matched RefusedVerb row, which "
                                                           "carries no counter column; and RefusedVerbs is empty, so "
                                                           "this arm is unreachable today" },
                                            row->why);

        // A cache verb at the scheduler's port. Answered rather than dropped, so a
        // misconfigured client learns which port it got wrong instead of seeing
        // something indistinguishable from a dead host.
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::DispatchNotPermitted,
                            .counter = IMetricsSink::Counter::DispatchFramesRefusedNotPermitted });
    }

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        // **Counted, and NOT the same refusal as the service's uncounted
        // `MalformedFrame`.** That one is a payload the service could not parse, and it
        // is deliberately uncounted so a broken client's noise stays out of the numbers
        // a fleet is sized from. This one is a frame whose own header disagrees with
        // what arrived, before any verb is routed. Two refusals, one wire code -- which
        // is precisely why the ROW is the unit and a table keyed on the code could not
        // express it (#327).
        return Cc::Refuse(_metrics,
                          { .code = Wire::ErrorCode::MalformedFrame,
                            .counter = IMetricsSink::Counter::DispatchFramesRefusedMalformedPayload });

    auto const reply = Route(descriptor->code, payload, caller);
    if (reply.status == Wire::Status::Ok)
        return Wire::EncodeReply(Wire::Status::Ok, reply.payload);
    // A counter here would double the six the service counts and pull the seven it
    // does not into series their own reasoning kept them out of.
    return Cc::RefuseWithoutCounter(
        { .code = reply.error,
          .rationale = "counted at the decision, in SchedulerService::Refuse, which triages every code it can produce "
                       "into RefusalTable or UncountedRefusals -- proven complete by RefusalsAreDisjoint()" },
        reply.message);
}

namespace
{
    /// Turn a per-tier domain table into the wire's positional list.
    ///
    /// The wire carries tiers by POSITION, because `CompileCacheWire.hpp` is
    /// compiled into `fastcache-cc` and cannot see `StorageTier`. This function
    /// and its inverse are the only two places that know the two orders are the
    /// same one, which is why they sit together — a transposition here is
    /// invisible to every compiler and to both ends of the wire.
    /// @param table One entry per tier, in enumerator order.
    /// @param project Turns one domain entry into its wire form.
    /// @return The same entries, positionally.
    template <typename Wired, typename Domain, typename Project>
    [[nodiscard]] Wire::PerTier<Wired> TiersToWire(Domain const& table, Project project)
    {
        Wire::PerTier<Wired> out;
        out.reserve(table.size());
        for (auto const& row: StorageTierTable)
        {
            auto const& entry = table[static_cast<std::size_t>(row.tier)];
            out.push_back(entry.has_value() ? std::optional { project(*entry) } : std::nullopt);
        }
        return out;
    }

    /// Read the wire's positional list back into a per-tier domain table.
    ///
    /// A position this build has no tier for is dropped rather than refused: it
    /// comes from a peer that knows a tier this one does not, and a cache figure
    /// is reported, never acted on, so there is nothing to fail closed about. A
    /// position the peer did not send stays absent, which is "no such tier".
    /// @param tiers The wire's list.
    /// @param project Turns one wire entry into its domain form.
    /// @return One entry per tier this build knows.
    template <typename Table, typename Wired, typename Project>
    [[nodiscard]] Table TiersFromWire(Wired const& tiers, Project project)
    {
        Table out {};
        for (auto const& row: StorageTierTable)
        {
            auto const index = static_cast<std::size_t>(row.tier);
            if (index >= tiers.size())
                continue;
            // Bound to a reference before it is tested, rather than subscripted
            // twice in one condition. The two spellings mean the same thing and
            // `bugprone-unchecked-optional-access` can only follow the first: a
            // second `tiers[index]` is a fresh expression it has no guard for, and
            // with `WarningsAsErrors` that is a build failure rather than a note.
            auto const& wired = tiers[index];
            if (wired.has_value())
                out[index] = project(*wired);
        }
        return out;
    }
} // namespace

NodeCacheCapacity CacheCapacityFromWire(Wire::CacheCapacityFields const& fields)
{
    return NodeCacheCapacity { .tierBytesLimit = TiersFromWire<EnumTable<StorageTier, std::optional<std::uint64_t>>>(
                                   fields.tiers, [](Wire::CacheTierBudget const& budget) { return budget.bytesLimit; }) };
}

Wire::CacheCapacityFields CacheCapacityToWire(NodeCacheCapacity const& cache)
{
    return Wire::CacheCapacityFields { .tiers =
                                           TiersToWire<Wire::CacheTierBudget>(cache.tierBytesLimit, [](std::uint64_t limit) {
                                               return Wire::CacheTierBudget { .bytesLimit = limit };
                                           }) };
}

NodeCacheLoad CacheLoadFromWire(Wire::CacheLoadFields const& fields)
{
    return NodeCacheLoad { .tiers = TiersFromWire<EnumTable<StorageTier, std::optional<CacheTierUsage>>>(
                               fields.tiers,
                               [](Wire::CacheTierUsage const& usage) {
                                   return CacheTierUsage { .itemCount = usage.itemCount,
                                                           .bytesUsed = usage.bytesUsed,
                                                           .evictions = usage.evictions,
                                                           .indexBytes = usage.indexBytes };
                               }),
                           .hits = fields.hits,
                           .misses = fields.misses };
}

Wire::CacheLoadFields CacheLoadToWire(NodeCacheLoad const& cache)
{
    return Wire::CacheLoadFields { .tiers = TiersToWire<Wire::CacheTierUsage>(
                                       cache.tiers,
                                       [](CacheTierUsage const& usage) {
                                           return Wire::CacheTierUsage { .itemCount = usage.itemCount,
                                                                         .bytesUsed = usage.bytesUsed,
                                                                         .evictions = usage.evictions,
                                                                         .indexBytes = usage.indexBytes };
                                       }),
                                   .hits = cache.hits,
                                   .misses = cache.misses };
}

Wire::CapacityFields CapacityToWire(NodeCapacity const& capacity)
{
    return Wire::CapacityFields { .logicalCores = capacity.logicalCores,
                                  .totalMemoryBytes = capacity.totalMemoryBytes,
                                  .nodeClassRaw = static_cast<std::uint8_t>(capacity.nodeClass),
                                  // Absent, not zero: the wire has to carry the
                                  // difference between "reserve nothing" and "use
                                  // whatever the class reserves", or the receiver
                                  // cannot tell a deliberate zero from silence.
                                  .reservedCores = capacity.reserveIsExplicit
                                                       ? std::optional<std::uint32_t> { capacity.reservedCores }
                                                       : std::nullopt,
                                  .cache = CacheCapacityToWire(capacity.cache),
                                  .reservedMemoryBytes = capacity.reservedMemoryBytes };
}

std::optional<NodeCapacity> CapacityFromWire(Wire::CapacityFields const& fields)
{
    auto const nodeClass = NodeClassFromRaw(fields.nodeClassRaw);
    if (!nodeClass.has_value())
        return std::nullopt;
    return NodeCapacity { .logicalCores = fields.logicalCores,
                          .totalMemoryBytes = fields.totalMemoryBytes,
                          .reservedMemoryBytes = fields.reservedMemoryBytes,
                          .nodeClass = *nodeClass,
                          .reservedCores = fields.reservedCores.value_or(0),
                          .reserveIsExplicit = fields.reservedCores.has_value(),
                          .cache = CacheCapacityFromWire(fields.cache) };
}

Wire::LoadFields LoadToWire(NodeLoad const& load)
{
    // `history` is stated and left empty: a node's own buckets are attached by the
    // heartbeat loop, which is the only thing that knows how far this machine has
    // handed its series over. Named rather than defaulted because clang-tidy fails
    // the build on a designated initializer that skips a field -- which is how a
    // field added to this record would otherwise be silently dropped here.
    return Wire::LoadFields { .cpuBusyPermille = load.cpuBusyPermille,
                              .availableMemoryBytes = load.availableMemoryBytes,
                              .freeScratchBytes = load.freeScratchBytes,
                              .cache = CacheLoadToWire(load.cache),
                              .history = {} };
}

NodeLoad LoadFromWire(Wire::LoadFields const& fields, std::uint32_t inFlight)
{
    return NodeLoad { .inFlight = inFlight,
                      .cpuBusyPermille = fields.cpuBusyPermille,
                      .availableMemoryBytes = fields.availableMemoryBytes,
                      .freeScratchBytes = fields.freeScratchBytes,
                      .cache = CacheLoadFromWire(fields.cache) };
}

SchedulerReply SchedulerProtocol::Route(Wire::Op op, std::span<std::byte const> payload, CallerContext const& caller)
{
    switch (op)
    {
        case Wire::Op::Register: {
            auto const fields = Wire::DecodeRegisterPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            // A class byte this build does not know means a peer built with a class
            // this one lacks. Guessing either way is a fleet decision made silently
            // -- see `NodeClassFromRaw` -- so it is refused where it can be said.
            auto const capacity = CapacityFromWire(fields->capacity);
            if (!capacity.has_value())
                return SchedulerReply::Malformed("this scheduler does not know that node class");
            return _service.Register(caller,
                                     WorkerRegistration { .fingerprint = Wire::AsStringView(fields->fingerprint),
                                                          .endpoint = Wire::AsStringView(fields->endpoint),
                                                          // Off the nested capacity record because that is the
                                                          // one extensible carrier REGISTER has -- its top-level
                                                          // arity is exact and fixed forever -- but kept out of
                                                          // `NodeCapacity`, which must stay a literal type.
                                                          .version = fields->capacity.version,
                                                          .toolchainLabel = fields->capacity.toolchainLabel,
                                                          .slots = fields->slots,
                                                          .codecs = fields->acceptedCodecs,
                                                          .capacity = *capacity });
        }
        case Wire::Op::Heartbeat: {
            auto const fields = Wire::DecodeHeartbeatPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Heartbeat(caller,
                                      Wire::AsStringView(fields->workerId),
                                      LoadFromWire(fields->load, fields->inFlight),
                                      HistoryFromWire(fields->load.history));
        }
        case Wire::Op::Lease: {
            auto const fields = Wire::DecodeLeasePayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Lease(caller,
                                  Wire::LeaseRequest { .fingerprint = Wire::AsStringView(fields->fingerprint),
                                                       .key = Wire::AsStringView(fields->key),
                                                       .acceptedCodecs = fields->acceptedCodecs });
        }
        case Wire::Op::Release: {
            auto const fields = Wire::DecodeReleasePayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Release(caller, Wire::AsStringView(fields->leaseToken), Wire::AsStringView(fields->key));
        }
        case Wire::Op::ClusterStatus:
            // A payload that is not empty is still refused, and by the same rule as
            // every other verb: the table says this one carries no fields, so
            // `SplitFields` accepts nothing but an empty payload. A request with
            // something in it is a client this build does not understand.
            if (!Wire::SplitFields(payload, Wire::OpFieldCount(Wire::Op::ClusterStatus)).has_value())
                return SchedulerReply::Malformed();
            return _service.ClusterStatus(caller);

        case Wire::Op::ClusterSet: {
            auto const fields = Wire::DecodeClusterSetPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.ClusterSet(caller, Wire::AsStringView(fields->name), Wire::AsStringView(fields->value));
        }

        case Wire::Op::ClusterForget: {
            auto const memberId = Wire::DecodeClusterForgetPayload(payload);
            if (!memberId.has_value())
                return SchedulerReply::Malformed();
            return _service.ClusterForget(caller, Wire::AsStringView(*memberId));
        }

        case Wire::Op::ClusterAdmit: {
            auto const fields = Wire::DecodeClusterAdmitPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.ClusterAdmit(
                caller, Wire::AsStringView(fields->memberId), Wire::AsStringView(fields->raftEndpoint));
        }
        default:
            // Unreachable: `IsSchedulerVerb` has already refused everything else.
            // Kept as a refusal rather than an assertion because a verb added to
            // `SchedulerOps` without a case here must fail closed, which is the
            // whole reason the table drives the test.
            return SchedulerReply {
                .status = Wire::Status::Error, .error = Wire::ErrorCode::DispatchNotPermitted, .message = {}, .payload = {}
            };
    }
}

std::vector<CompileCacheWire::HistoryBucketFields> HistoryToWire(std::span<FleetBucket const> buckets)
{
    std::vector<CompileCacheWire::HistoryBucketFields> out;
    out.reserve(buckets.size());
    for (auto const& bucket: buckets)
    {
        auto& record = out.emplace_back();
        record.startMillis = static_cast<std::uint64_t>(bucket.startMillis);
        record.sampleMillis = static_cast<std::uint64_t>(bucket.sampleMillis);
        // One assignment: `EnumTable` IS `std::array`, and the static_assert in the
        // header ties the two extents, so these are the same type.
        record.values = bucket.values;
    }
    return out;
}

std::vector<FleetBucket> HistoryFromWire(std::span<CompileCacheWire::HistoryBucketFields const> records)
{
    std::vector<FleetBucket> out;
    out.reserve(records.size());
    for (auto const& record: records)
    {
        auto& bucket = out.emplace_back();
        bucket.startMillis = static_cast<std::int64_t>(record.startMillis);
        bucket.sampleMillis = static_cast<std::int64_t>(record.sampleMillis);
        bucket.values = record.values;
        bucket.present = true;
    }
    return out;
}

} // namespace FastCache::Distributed
