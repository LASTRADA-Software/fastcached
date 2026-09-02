// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"

#include <FastCache/CompileCache/CompileValue.hpp>
#include <FastCache/Protocol/SurfaceRefusal.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string_view>

namespace FastCache::Node
{

namespace
{
    namespace Wire = CompileCacheWire;

    /// What this tier does about each refusal it answers
    /// ([#491](https://github.com/LASTRADA-Software/fastcached/issues/491)).
    ///
    /// Rows rather than arguments at the call sites, for the reason `CompileRefusal`
    /// is: a row pairs the wire code a client acts on with the counter an operator
    /// watches, so there is no argument to pass a bare `ErrorCode` to and the counter
    /// cannot be left out. The uncounted ones carry a `rationale` instead, which is
    /// the same forcing function pointing the other way -- the author cannot write the
    /// call without answering "would a rise here mean something happened".
    ///
    /// **The test, so a NEW arm can be classified without reading these eight.** A
    /// refusal is counted when a rise in it names something an operator would go and
    /// do something about; it is uncounted when a rise would be *ordinary traffic*,
    /// when the arm cannot fire at all, or when the same event is already counted
    /// somewhere better placed to see it. An arm nobody has applied the test to yet is
    /// neither: it is `Cc::RefuseUntriaged` with the issue that will decide it, which
    /// `worker-refusals-counted` tallies and prints on every run, and which fails the
    /// build outright when no issue can be resolved for the file.
    namespace TierRefusal
    {
        /// A request at a wire version this build cannot decode.
        ///
        /// **Counted.** A client compiled against another release fails every exchange
        /// it attempts, and the only other evidence is a cache that looks permanently
        /// cold: `fastcache-cc` reads a refused `FETCH` as "this daemon is not worth a
        /// second command", steps over it and compiles locally, so the build stays
        /// correct and merely stops being fast. That is the failure shape this tree
        /// has already paid for twice.
        constexpr Cc::SurfaceRefusal UnsupportedVersion {
            .code = Wire::ErrorCode::UnsupportedVersion,
            .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedUnsupportedVersion,
        };

        /// A `FETCH` or `STORE` body that would not decode.
        ///
        /// **Counted**, and one row for both verbs: they carry different fields and
        /// they say the same thing about the peer, which is that two ends agreeing on
        /// the framing disagree about what goes inside it. An operator does one thing
        /// about that -- find the client that is out of step -- so splitting them would
        /// be a distinction nothing acts on, which is the mirror of the mistake that
        /// sums two refusals an operator acts on differently.
        ///
        /// Its OWN counter rather than any other `malformed-frame` row. The code is
        /// shared with a truncated compile frame, an undecodable compile payload and
        /// two `AUTH` payloads; the row is the refusal and not the code.
        constexpr Cc::SurfaceRefusal MalformedPayload {
            .code = Wire::ErrorCode::MalformedFrame,
            .counter = IMetricsSink::Counter::NodeCacheRequestsRefusedMalformedPayload,
        };

        /// A frame whose payload is not the length its header declared.
        ///
        /// **Uncounted, because it cannot fire through the listener.**
        /// `FrameEndpoint` assembles the frame from `ReadExactly(payloadLength)`, so
        /// the two figures are one figure by construction. It stays as defence in
        /// depth for a direct `Answer` -- this class is pure and reachable that way --
        /// and a counter for an arm the transport makes unreachable would be a row in
        /// a table whose whole value is that every row means something.
        constexpr Cc::UncountedRefusal Truncated {
            .code = Wire::ErrorCode::MalformedFrame,
            .rationale = "FrameEndpoint reads exactly the declared length, so the payload cannot be a different "
                         "size; defence in depth for a direct Answer, not an arm a peer can reach",
        };

        /// A third header byte with no `OpTable` row.
        ///
        /// **Uncounted, for the same reason and a stronger one.** `MergedResponder`
        /// hands a frame to this tier only when `FamilyOf` names the `Cache` family,
        /// and a byte with no row is `Unset` -- so "reached this tier" and "has no row"
        /// are mutually exclusive by the definition of `FamilyOf`, not by a routing
        /// decision that could be revisited. An unknown opcode is answered
        /// `UnservedReply` at the door, and it is counted nowhere for the reason
        /// stated there.
        constexpr Cc::UncountedRefusal UnknownOpcode {
            .code = Wire::ErrorCode::UnknownOpcode,
            .rationale = "FamilyOf gives an opcode with no OpTable row the Unset family, which MergedResponder owns "
                         "nowhere, so a frame reaching this tier always names a verb this build knows",
        };

        /// A local write that failed.
        ///
        /// **Uncounted here, and counted one layer down**: `LocalCache::Store` moves
        /// `NodeCacheStoreFailures` at the write itself. A row here would count one
        /// failed write twice, and the lower one is the better placed of the two -- it
        /// sees every caller, where this arm sees only the callers that arrived over
        /// the wire.
        ///
        /// It is also not a refusal OF the peer. Nothing the client sent was wrong;
        /// this tier could not keep what it was given, which is why the code says
        /// storage and not framing.
        constexpr Cc::UncountedRefusal StorageWriteFailed {
            .code = Wire::ErrorCode::StorageWriteFailed,
            .rationale = "LocalCache::Store already counts this as NodeCacheStoreFailures at the write, where every "
                         "caller is visible and not only the ones that arrived over the wire",
        };

        /// Why a verb named by `RefusedVerbs` -- today, `AUTH` -- counts nothing.
        ///
        /// **The arm where a counter would be actively harmful.** It is what a
        /// `FASTCACHE_TOKEN`-configured launcher produces once per exchange for a whole
        /// build, so the series would be dominated by healthy traffic and a port scan
        /// would be invisible inside it -- the same argument
        /// `MergedResponder::UnservedReply` carries, which is why #447 withdrew a
        /// counter of its own rather than shipping one that could not be read.
        ///
        /// Unreachable through the listener besides: `AUTH` is the `Session` family,
        /// routed to the scheduler or answered unserved. Either clause alone settles
        /// it; both are recorded because the first survives a routing change and the
        /// second does not.
        ///
        /// A bare rationale and **not** a `Cc::UncountedRefusal`, unlike every other
        /// row here: the code this arm answers with comes from the matched
        /// `Wire::RefusedVerb` row, so a `code` member beside this text would be built,
        /// documented and never read -- a field that looks load-bearing because its
        /// neighbours are.
        constexpr std::string_view RefusedVerbRationale =
            "a token-configured launcher sends AUTH once per exchange for a whole build, so this series would be "
            "dominated by healthy traffic and a scan invisible inside it";

        /// A scheduler or compile verb at the cache tier.
        ///
        /// **Uncounted**: `MergedResponder` routes by verb family, so a frame reaching
        /// this tier is a cache verb and this arm is unreachable through the listener.
        /// It stays because `Answer` is reachable directly and because the sentence it
        /// carries is the right one for a client that dialled a node built without the
        /// component it wanted.
        constexpr Cc::UncountedRefusal WrongSurface {
            .code = Wire::ErrorCode::DispatchNotPermitted,
            .rationale = "MergedResponder routes by verb family, so a frame reaching this tier names a cache verb; "
                         "this arm answers a direct call and nothing a peer can send",
        };
    } // namespace TierRefusal

    /// This surface's rows. The shape, the lookup and why they exist are on
    /// `Wire::RefusedVerb`; what belongs here is only which verbs and what they say.
    ///
    /// `Auth`, because a `FASTCACHE_TOKEN` launcher had a permanent 0% hit rate that
    /// presented exactly as a cache that is merely cold.
    constexpr std::array RefusedVerbs {
        Wire::RefusedVerb { .op = Wire::Op::Auth,
                            .code = Wire::UnimplementedVerb,
                            .why = "this endpoint is the node's cache and checks no credential" },
    };

    // The table is consulted from the `default:` arm only, so a row naming FETCH or
    // STORE would sit there looking like a decision and change nothing. Refused at
    // compile time rather than left to be noticed.
    static_assert(std::ranges::none_of(
                      RefusedVerbs,
                      [](Wire::Op op) { return op == Wire::Op::Fetch || op == Wire::Op::Store; },
                      &Wire::RefusedVerb::op),
                  "a refusal row for a verb this tier serves is dead: the lookup never reaches it");
} // namespace

Task<std::vector<std::byte>> CacheProxy::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: with no declared length there is nowhere to resynchronize to,
        // so there is nothing an answer could mean. The only condition that closes.
        co_return std::vector<std::byte> {};

    if (!Wire::IsSupported(header->version))
        co_return Cc::Refuse(_metrics,
                             TierRefusal::UnsupportedVersion,
                             std::format("supported versions {}..{}",
                                         static_cast<unsigned>(Wire::MinSupportedVersion),
                                         static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        co_return Cc::RefuseWithoutCounter(TierRefusal::UnknownOpcode);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        co_return Cc::RefuseWithoutCounter(TierRefusal::Truncated);

    switch (descriptor->code)
    {
        case Wire::Op::Fetch: {
            auto const key = Wire::DecodeFetchPayload(payload);
            if (!key.has_value())
                co_return Cc::Refuse(_metrics, TierRefusal::MalformedPayload);

            auto const found = co_await _cache.Fetch(Wire::AsStringView(*key));
            if (!found.has_value())
                // A miss is `Miss` with a zero-length payload, never `Error`. The two
                // being one byte is a defect this wire has already recorded paying
                // for: a rejected client saw an endlessly cold cache and no
                // diagnostic, and the build merely got slower forever.
                co_return Wire::EncodeReply(Wire::Status::Miss, {});
            co_return Wire::EncodeReply(Wire::Status::Ok, *found);
        }
        case Wire::Op::Store: {
            auto const fields = Wire::DecodeStorePayload(payload);
            if (!fields.has_value())
                co_return Cc::Refuse(_metrics, TierRefusal::MalformedPayload);

            // Canonicalized against the roots the client sent, through the one
            // recipe both servers on this wire share.
            //
            // This block used to ignore those roots, on the reasoning that
            // canonicalization is "the SHARED cache's job" and that "what this tier
            // stores is what this machine will replay". Both were true when a node
            // was a private tier in front of `fastcached`. #229 made a node the
            // shared cache, and "this machine" is not one layout -- every checkout on
            // it is a different one -- so a value stored here kept its producer's
            // absolute paths and every consumer replayed them into its build system's
            // dependency graph (#319).
            //
            // A value that does not decode is stored VERBATIM rather than refused,
            // which is where this server's policy differs from the daemon's: an
            // opaque value is not this tier's business to reject. `CanonicalStoredValue`
            // answers `nullopt` and each server says what it wants to.
            auto const canonical = CanonicalStoredValue(
                fields->value, Wire::AsStringView(fields->srcRoot), Wire::AsStringView(fields->buildTree));
            auto const toStore = canonical.has_value() ? std::span<std::byte const> { *canonical } : fields->value;

            if (!co_await _cache.Store(Wire::AsStringView(fields->key), toStore))
                co_return Cc::RefuseWithoutCounter(TierRefusal::StorageWriteFailed);
            co_return Wire::EncodeReply(Wire::Status::Ok, {});
        }
        default:
            if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
                // `row->why` is the sentence the CLIENT is sent; `rationale` on the row
                // above is why nothing rises and is never transmitted. The two meet in
                // this one expression, which is exactly where the names have to differ.
                co_return Cc::RefuseWithoutCounter({ .code = row->code, .rationale = TierRefusal::RefusedVerbRationale },
                                                   row->why);

            // A scheduler or worker verb at the cache port. Answered rather than
            // dropped, so a client that reached the wrong one of this node's ports
            // learns which instead of seeing something indistinguishable from a dead
            // host.
            co_return Cc::RefuseWithoutCounter(TierRefusal::WrongSurface,
                                               "this endpoint is the node's cache; scheduling and compiles are served "
                                               "on their own ports");
    }
}

} // namespace FastCache::Node
