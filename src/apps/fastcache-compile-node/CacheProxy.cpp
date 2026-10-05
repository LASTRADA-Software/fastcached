// SPDX-License-Identifier: Apache-2.0
#include "CacheProxy.hpp"
#include "PrivateTierProfile.hpp"
#include "SharedTierProfile.hpp"

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
    /// **The test, so a NEW arm can be classified without working backwards from the
    /// arms below.** A refusal is counted when a rise in it names something an
    /// operator would go and act on; it is uncounted when a rise would be *ordinary
    /// traffic*, when the arm cannot fire at all, or when the same event is already
    /// counted somewhere better placed to see it. An arm nobody has applied the test
    /// to yet is neither: it is `Cc::RefuseUntriaged` carrying the issue that will
    /// decide it, which `worker-refusals-counted` tallies and prints on every run,
    /// and which fails the build outright when no issue can be resolved for the file.
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
        ///
        /// A function of the tier rather than a constant: each tier counts it on its own series.
        /// @param tier The tier answering.
        /// @return The row, naming that tier's counter.
        [[nodiscard]] constexpr Cc::SurfaceRefusal UnsupportedVersion(CacheTierProfile const& tier) noexcept
        {
            return { .code = Wire::ErrorCode::UnsupportedVersion, .counter = tier.refusedUnsupportedVersion };
        }

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
        ///
        /// A function of the tier, for `UnsupportedVersion`'s reason.
        /// @param tier The tier answering.
        /// @return The row, naming that tier's counter.
        [[nodiscard]] constexpr Cc::SurfaceRefusal MalformedPayload(CacheTierProfile const& tier) noexcept
        {
            return { .code = Wire::ErrorCode::MalformedFrame, .counter = tier.refusedMalformedPayload };
        }

        /// A `STORE` whose value names a canonicalization generation this build does
        /// not implement.
        ///
        /// **Counted**, and the ground matters because the obvious one did not hold.
        /// "`CompileValueVersion` has never moved, so the baseline is zero" argued for
        /// the series from the absence of the thing it counts, and it expired exactly
        /// when this counter started doing work: #547 bumped the byte to 2, so
        /// generation-1 values are in the field and a rollout across that bump is the
        /// first thing this series ever sees. It stood for one generation and then
        /// said nothing, which is why the ground below is the one that is written down.
        ///
        /// The ground that survives a bump: **a converged fleet produces none.** That
        /// is what separates it from the `AUTH` arm below, which a correctly
        /// configured launcher emits once per exchange forever, so no scan can ever be
        /// seen inside that series. This one is zero, then a burst while a rollout is
        /// in flight — once per TU, which is a lot — then zero again. A series that
        /// returns to zero is readable however tall the burst was, and the burst is
        /// the event. Against the third test: nothing else sees it, since
        /// `LocalCache::Store` counts writes that FAILED and this is a write never
        /// attempted.
        ///
        /// Reachable by the ordinary path rather than a direct `Answer`: it is what a
        /// launcher at another generation produces, and a fleet is permanently
        /// mid-upgrade
        /// ([#173](https://github.com/LASTRADA-Software/fastcached/issues/173)).
        ///
        /// What an operator does about it is what they do about the version row above
        /// — find the machine that is out of step, finish or roll back — and it is the
        /// only view of what #483's decision costs, since refusing buys correctness by
        /// giving up hits and the launcher reports only a miss.
        ///
        /// The CODE is its own, and this surface needed it more than the daemon did
        /// (#544). It was `MalformedValue`, which told a launcher its cache was
        /// damaged while the fleet was merely mid-rollout — and since #229 a node IS
        /// the shared cache, so this is the surface a launcher actually talks to and
        /// therefore where that wrong reading was reached.
        ///
        /// A function of the tier, for `UnsupportedVersion`'s reason.
        /// @param tier The tier answering.
        /// @return The row, naming that tier's counter.
        [[nodiscard]] constexpr Cc::SurfaceRefusal ForeignGeneration(CacheTierProfile const& tier) noexcept
        {
            return { .code = Wire::ErrorCode::ForeignValueGeneration, .counter = tier.refusedForeignGeneration };
        }

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
        /// **Uncounted here, and counted one layer down**: `LocalCache::Store` moves the
        /// tier's `storeFailures` at the write itself. A row here would count one
        /// failed write twice, and the lower one is the better placed of the two -- it
        /// sees every caller, where this arm sees only the callers that arrived over
        /// the wire.
        ///
        /// It is also not a refusal OF the peer. Nothing the client sent was wrong;
        /// this tier could not keep what it was given, which is why the code says
        /// storage and not framing.
        constexpr Cc::UncountedRefusal StorageWriteFailed {
            .code = Wire::ErrorCode::StorageWriteFailed,
            .rationale = "LocalCache::Store already counts this as the tier's storeFailures at the write, where "
                         "every caller is visible and not only the ones that arrived over the wire",
        };

        /// A removal the tier could not persist.
        ///
        /// **Uncounted here, and counted one layer down**, for `StorageWriteFailed`'s reason:
        /// `WriteErrorReportingStorage` sits under every node tier and reports a
        /// persistence-class failure on a removal as it does on a write -- one `Warn` line
        /// and `fastcached_write_errors_total` -- where every caller is visible. The code is
        /// the write code, because a removal IS a write to the store and the remedy is the
        /// same disk.
        constexpr Cc::UncountedRefusal StorageRemoveFailed {
            .code = Wire::ErrorCode::StorageWriteFailed,
            .rationale = "WriteErrorReportingStorage already reports this as fastcached_write_errors_total at the "
                         "removal, where every caller is visible and not only the ones that arrived over the wire",
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
        /// routed to the session component on every built node. Either clause alone
        /// settles it; both are recorded because the first survives a routing change and
        /// the second does not.
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

        /// A verb the node's OTHER cache tier answers: `FETCH` at the shared tier, `SHARED-FETCH`
        /// at the private one.
        ///
        /// `DispatchNotPermitted`, because the verb IS implemented -- by the other tier on this
        /// same node -- and that code is how this wire says *served elsewhere*.
        /// `UnimplementedVerb` is reserved for a verb nothing implements, which a client reads as
        /// *this build is too old*. The sentence names the other tier, so the refusal is told
        /// apart from `WrongSurface`'s, which shares the code and names the other PORTS.
        ///
        /// A refusal DERIVED from `TierProfiles` rather than a `RefusedVerbs` row: the verbs it
        /// covers are exactly the ones some other tier's profile names, so a table of them would
        /// be a second copy of the profiles that could drift from them.
        ///
        /// **Uncounted**, for `WrongSurface`'s reason: `MergedResponder` sends each cache family to
        /// the tier that serves it, so a frame reaches a tier naming its twin's verb only through a
        /// direct `Answer`, never from a peer.
        constexpr Cc::UncountedRefusal TwinTierVerb {
            .code = Wire::ErrorCode::DispatchNotPermitted,
            .rationale = "MergedResponder routes each cache family to the tier whose profile names it, so a tier "
                         "sees its twin's verbs only through a direct Answer, never from a peer",
        };
    } // namespace TierRefusal

    /// What one verb is to one tier. PRIVATE: never transmitted and never persisted.
    enum class TierVerb : std::uint8_t
    {
        Fetch,     ///< The profile's fetch verb.
        Store,     ///< The profile's store verb.
        Drop,      ///< `CacheDrop`, on a profile that serves it.
        NotServed, ///< Anything else.
    };

    /// Which of the tier's arms @p op reaches.
    ///
    /// The profile's columns read as the arms of `Answer`'s switch, in one place, so the switch
    /// and the `static_assert` below ask the same question.
    /// @param tier The tier answering.
    /// @param op The verb, already resolved against `OpTable`.
    /// @return The arm, or `NotServed`.
    [[nodiscard]] constexpr TierVerb ServedAs(CacheTierProfile const& tier, Wire::Op op) noexcept
    {
        if (op == tier.verbs.fetch)
            return TierVerb::Fetch;
        if (op == tier.verbs.store)
            return TierVerb::Store;
        if (op == Wire::Op::CacheDrop && tier.drop == DropVerb::Serves)
            return TierVerb::Drop;
        return TierVerb::NotServed;
    }

    /// Every tier a node builds, so a verb one of them serves is refused by the others as theirs
    /// to answer, never as a scheduler or compile verb.
    constexpr std::array TierProfiles { PrivateTierProfile, SharedTierProfile };

    /// Whether some tier's profile names @p op.
    /// @param op The verb.
    /// @return True when a tier answers it.
    [[nodiscard]] constexpr bool ServedByATier(Wire::Op op) noexcept
    {
        return std::ranges::any_of(TierProfiles,
                                   [op](CacheTierProfile const& tier) { return ServedAs(tier, op) != TierVerb::NotServed; });
    }

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

    // The table is consulted from the `NotServed` arm only, so a row naming a verb a tier
    // serves would sit there looking like a decision and change nothing on that tier.
    // Refused at compile time, over every tier's profile, rather than left to be noticed.
    static_assert(std::ranges::none_of(RefusedVerbs, ServedByATier, &Wire::RefusedVerb::op),
                  "a refusal row for a verb a tier serves is dead: the lookup never reaches it");
} // namespace

core::async::Task<std::vector<std::byte>> CacheProxy::Answer(std::span<std::byte const> frame)
{
    auto const header = Wire::DecodeRequestHeader(frame);
    if (!header.has_value())
        // Wrong magic: with no declared length there is nowhere to resynchronize to,
        // so there is nothing an answer could mean. The only condition that closes.
        co_return std::vector<std::byte> {};

    if (!Wire::IsSupported(header->version))
        co_return Cc::Refuse(_metrics,
                             TierRefusal::UnsupportedVersion(_cache.Profile()),
                             std::format("supported versions {}..{}",
                                         static_cast<unsigned>(Wire::MinSupportedVersion),
                                         static_cast<unsigned>(Wire::CurrentVersion)));

    auto const* descriptor = Wire::FindOp(header->opRaw);
    if (descriptor == nullptr)
        co_return Cc::RefuseWithoutCounter(TierRefusal::UnknownOpcode);

    auto const payload = frame.subspan(Wire::RequestHeaderSize);
    if (payload.size() != header->payloadLength)
        co_return Cc::RefuseWithoutCounter(TierRefusal::Truncated);

    switch (ServedAs(_cache.Profile(), descriptor->code))
    {
        case TierVerb::Fetch: {
            auto const key = Wire::DecodeFetchPayload(payload);
            if (!key.has_value())
                co_return Cc::Refuse(_metrics, TierRefusal::MalformedPayload(_cache.Profile()));

            auto const found = co_await _cache.Fetch(Wire::AsStringView(*key));
            if (!found.has_value())
                // A miss is `Miss` with a zero-length payload, never `Error`. The two
                // being one byte is a defect this wire has already recorded paying
                // for: a rejected client saw an endlessly cold cache and no
                // diagnostic, and the build merely got slower forever.
                co_return Wire::EncodeReply(Wire::Status::Miss, {});
            co_return Wire::EncodeReply(Wire::Status::Ok, *found);
        }
        case TierVerb::Store: {
            auto const fields = Wire::DecodeStorePayload(payload);
            if (!fields.has_value())
                co_return Cc::Refuse(_metrics, TierRefusal::MalformedPayload(_cache.Profile()));

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
            // Bytes that are not a stored value at all are stored VERBATIM rather
            // than refused, which is where this server's policy differs from the
            // daemon's: an opaque value is not this tier's business to reject.
            //
            // A value of another GENERATION is not in that category and this tier has
            // no choice about it (#483). It used to be, because `CanonicalStoredValue`
            // answered one `nullopt` for both -- so a launcher at generation N storing
            // into a node at N+1 landed on the verbatim arm and put the producing
            // checkout's absolute paths into the shared cache under a key every
            // machine computes, which is #229/#319 reached by nothing worse than a
            // rolling upgrade. The switch is what protects: `bytes` is empty for the
            // foreign case, but the fallback here was never the canonical bytes, so
            // `outcome == Canonicalized ? canonical.bytes : fields->value` would
            // compile, read naturally, and reinstate the whole thing.
            auto const canonical = CanonicalStoredValue(
                fields->value, Wire::AsStringView(fields->srcRoot), Wire::AsStringView(fields->buildTree));

            std::span<std::byte const> toStore {};
            switch (canonical.outcome)
            {
                case CanonicalizationOutcome::Canonicalized:
                    toStore = canonical.bytes;
                    break;
                case CanonicalizationOutcome::NotACompileValue:
                    // The verbatim policy, at the case label that means it. It used
                    // to be the initializer above, which is the same separation the
                    // comment warns about in miniature: an arm reading `break;` says
                    // "nothing to do" where this one decides something.
                    toStore = fields->value;
                    break;
                case CanonicalizationOutcome::ForeignGeneration:
                    co_return Cc::Refuse(_metrics,
                                         TierRefusal::ForeignGeneration(_cache.Profile()),
                                         ForeignGenerationMessage(canonical.generation));
            }

            if (!co_await _cache.Store(Wire::AsStringView(fields->key), toStore))
                co_return Cc::RefuseWithoutCounter(TierRefusal::StorageWriteFailed);
            co_return Wire::EncodeReply(Wire::Status::Ok, {});
        }
        case TierVerb::Drop: {
            // The locality gate has already run: `CacheResponder::RefusePeer` answers for
            // every verb that reaches this tier, before the payload was read, so a caller
            // on another machine never gets here and the key it named is untouched.
            auto const key = Wire::DecodeCacheDropPayload(payload);
            if (!key.has_value())
                co_return Cc::Refuse(_metrics, TierRefusal::MalformedPayload(_cache.Profile()));

            switch (_cache.Drop(Wire::AsStringView(*key)))
            {
                case CacheDropOutcome::Removed:
                    co_return Wire::EncodeReply(Wire::Status::Ok, {});
                case CacheDropOutcome::Absent:
                    // `Miss`, never `Error`: nothing to remove is what a repair's second run
                    // finds, and see `Op::CacheDrop` for why that must not read as broken.
                    co_return Wire::EncodeReply(Wire::Status::Miss, {});
                case CacheDropOutcome::Failed:
                    co_return Cc::RefuseWithoutCounter(TierRefusal::StorageRemoveFailed);
            }
            co_return Cc::RefuseWithoutCounter(TierRefusal::StorageRemoveFailed);
        }
        case TierVerb::NotServed:
            break;
    }

    if (auto const* const row = Wire::FindRefusal(RefusedVerbs, descriptor->code); row != nullptr)
        // `row->why` is the sentence the CLIENT is sent; `rationale` on the row
        // above is why nothing rises and is never transmitted. The two meet in
        // this one expression, which is exactly where the names have to differ.
        co_return Cc::RefuseWithoutCounter({ .code = row->code, .rationale = TierRefusal::RefusedVerbRationale }, row->why);

    if (ServedByATier(descriptor->code))
        co_return Cc::RefuseWithoutCounter(
            TierRefusal::TwinTierVerb,
            "this cache tier answers its own verb pair; the node's other cache tier serves this one");

    // A scheduler or worker verb at the cache port. Answered rather than
    // dropped, so a client that reached the wrong one of this node's ports
    // learns which instead of seeing something indistinguishable from a dead
    // host.
    co_return Cc::RefuseWithoutCounter(TierRefusal::WrongSurface,
                                       "this endpoint is the node's cache; scheduling and compiles are served "
                                       "on their own ports");
}

} // namespace FastCache::Node
