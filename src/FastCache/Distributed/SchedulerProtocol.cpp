// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Distributed/SchedulerProtocol.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <type_traits>

namespace FastCache::Distributed
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// The verbs a scheduler answers.
    ///
    /// A table rather than three `case` labels so the membership test below is
    /// derived from it: a verb that reaches this class without a row is refused
    /// rather than served, which is the direction a mistake has to fail in.
    constexpr std::array SchedulerOps { Wire::Op::Register,      Wire::Op::Heartbeat,  Wire::Op::Lease,
                                        Wire::Op::ClusterStatus, Wire::Op::ClusterSet, Wire::Op::ClusterForget,
                                        Wire::Op::ClusterAdmit };

    /// Whether this scheduler serves @p op at all.
    /// @param op The verb, already resolved against `OpTable`.
    /// @return True when it is one of `SchedulerOps`.
    [[nodiscard]] constexpr bool IsSchedulerVerb(Wire::Op op) noexcept
    {
        return std::ranges::find(SchedulerOps, op) != SchedulerOps.end();
    }
} // namespace

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
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnsupportedVersion,
                                      std::format("supported versions {}..{}",
                                                  static_cast<unsigned>(Wire::MinSupportedVersion),
                                                  static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        // Stepped over rather than fatal: the framing exists so a receiver can skip
        // a verb it does not know, which is what lets a newer client talk to an
        // older scheduler at all.
        return Wire::EncodeErrorReply(Wire::ErrorCode::UnknownOpcode);

    if (!IsSchedulerVerb(descriptor->code))
        // A cache verb at the scheduler's port. Answered rather than dropped, so a
        // misconfigured client learns which port it got wrong instead of seeing
        // something indistinguishable from a dead host.
        return Wire::EncodeErrorReply(Wire::ErrorCode::DispatchNotPermitted);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        return Wire::EncodeErrorReply(Wire::ErrorCode::MalformedFrame);

    auto const reply = Route(descriptor->code, payload, caller);
    if (reply.status == Wire::Status::Ok)
        return Wire::EncodeReply(Wire::Status::Ok, reply.payload);
    return Wire::EncodeErrorReply(reply.error, reply.message);
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
                                                           .evictions = usage.evictions };
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
                                                                         .evictions = usage.evictions };
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
                                  .cache = CacheCapacityToWire(capacity.cache) };
}

std::optional<NodeCapacity> CapacityFromWire(Wire::CapacityFields const& fields)
{
    auto const nodeClass = NodeClassFromRaw(fields.nodeClassRaw);
    if (!nodeClass.has_value())
        return std::nullopt;
    return NodeCapacity { .logicalCores = fields.logicalCores,
                          .totalMemoryBytes = fields.totalMemoryBytes,
                          .nodeClass = *nodeClass,
                          .reservedCores = fields.reservedCores.value_or(0),
                          .reserveIsExplicit = fields.reservedCores.has_value(),
                          .cache = CacheCapacityFromWire(fields.cache) };
}

Wire::LoadFields LoadToWire(NodeLoad const& load)
{
    return Wire::LoadFields { .cpuBusyPermille = load.cpuBusyPermille,
                              .availableMemoryBytes = load.availableMemoryBytes,
                              .freeScratchBytes = load.freeScratchBytes,
                              .cache = CacheLoadToWire(load.cache) };
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
                                                          .slots = fields->slots,
                                                          .codecs = fields->acceptedCodecs,
                                                          .capacity = *capacity });
        }
        case Wire::Op::Heartbeat: {
            auto const fields = Wire::DecodeHeartbeatPayload(payload);
            if (!fields.has_value())
                return SchedulerReply::Malformed();
            return _service.Heartbeat(
                caller, Wire::AsStringView(fields->workerId), LoadFromWire(fields->load, fields->inFlight));
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

} // namespace FastCache::Distributed
