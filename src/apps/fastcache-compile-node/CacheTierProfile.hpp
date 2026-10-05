// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace FastCache::Node
{

/// The cache verb pair a profile row names, or UNSTATED.
///
/// Built only from a `CompileCacheWire::CacheVerbs`, whose `consteval` constructor proves the pair
/// has FETCH's and STORE's shapes. Default-constructed it is UNSTATED -- `stated` is false, which is
/// what a row that forgot its pair carries and what `StatesEveryMember` refuses. A flag rather than
/// an unassigned opcode, because `Op` has no zero enumerator to spell one with. A wrapper rather
/// than a default on the wire type itself, because a default-constructible `CacheVerbs` would be one
/// every encoder could be handed unproven.
struct TierVerbs
{
    CompileCacheWire::Op fetch { CompileCacheWire::Op::Fetch }; ///< The read verb; meaningless while unstated.
    CompileCacheWire::Op store { CompileCacheWire::Op::Store }; ///< The write verb; meaningless while unstated.
    bool stated { false };                                      ///< Whether a row named the pair.

    /// Unstated.
    constexpr TierVerbs() noexcept = default;

    /// @param verbs A pair already proved at compile time.
    explicit consteval TierVerbs(CompileCacheWire::CacheVerbs verbs) noexcept:
        fetch { verbs.fetch },
        store { verbs.store },
        stated { true }
    {
    }

    /// Field by field.
    [[nodiscard]] friend constexpr bool operator==(TierVerbs, TierVerbs) noexcept = default;
};

/// Whether a tier answers `CACHE-DROP`. **PRIVATE: in-process only**, never transmitted or
/// persisted, so it carries no values; `Unstated` is what a row that forgot the column carries.
enum class DropVerb : std::uint8_t
{
    Unstated, ///< Not a classification; `StatesEveryMember` refuses it.
    Serves,   ///< `CACHE-DROP` is answered.
    Refuses,  ///< `CACHE-DROP` is refused as the other tier's.
};

/// Which verbs one cache tier answers and which counters it moves, as one row.
///
/// A node runs two tiers over the same code: its private tier, which answers this machine's
/// `FETCH`/`STORE`/`CACHE-DROP`, and the fleet's shared tier, which answers
/// `SHARED-FETCH`/`SHARED-STORE`. They share `LocalCache` and `CacheProxy` deliberately, because a
/// stored value is canonicalized by every server on this wire and a second implementation of that
/// path is a second place for it to be got wrong. What they must NOT share is their counters: a
/// surface moving another's series is the merge failure that makes an operator's reading of either
/// one wrong, since the private tier's hit rate would then include every other machine's builds.
///
/// So the difference between the two is this descriptor and nothing else, passed REQUIRED and
/// undefaulted to both classes: a default would let a construction site that never thought about
/// which tier it was building inherit the private tier's counters in silence.
///
/// Each profile lives in the header of the surface whose counters it names --
/// `PrivateTierProfile.hpp` and `SharedTierProfile.hpp` -- so the counter attribution check, which
/// reads one surface per file, asks each one about its own surface.
///
/// PRIVATE: never transmitted and never persisted. Every member is one byte wide or a pair of
/// bytes, so the row carries no padding.
struct CacheTierProfile
{
    /// The fetch and store verbs `CacheProxy` answers. Any other cache verb is refused.
    TierVerbs verbs {};

    /// Whether `CACHE-DROP` is answered. The private tier only: a destructive verb reaches the
    /// endpoint its sender named, and a fleet caller must not delete from the fleet's tier.
    DropVerb drop { DropVerb::Unstated };

    /// A key served from this tier's own storage.
    IMetricsSink::Counter hits { IMetricsSink::Counter::Last };

    /// A key this tier's own storage did not hold, whatever the upstream then answered.
    IMetricsSink::Counter misses { IMetricsSink::Counter::Last };

    /// A local write that failed.
    IMetricsSink::Counter storeFailures { IMetricsSink::Counter::Last };

    /// A miss the upstream answered. Absent where the tier's upstream is `NoUpstream`, which never
    /// answers one: absent says *cannot happen here*, never *forgot*.
    std::optional<IMetricsSink::Counter> upstreamHits;

    /// A value the upstream answered that could not be kept locally. Absent where there is no
    /// upstream to answer one.
    std::optional<IMetricsSink::Counter> fillFailures;

    /// An object the upstream took. Absent where the upstream answers every store
    /// `NotConfigured`, which is uncounted.
    std::optional<IMetricsSink::Counter> upstreamStores;

    /// An object the upstream declined. Absent for `upstreamStores`'s reason.
    std::optional<IMetricsSink::Counter> upstreamStoreFailures;

    /// A request at a wire version this build cannot decode.
    IMetricsSink::Counter refusedUnsupportedVersion { IMetricsSink::Counter::Last };

    /// A fetch, store or drop body that would not decode.
    IMetricsSink::Counter refusedMalformedPayload { IMetricsSink::Counter::Last };

    /// A stored value naming a canonicalization generation this build does not implement.
    IMetricsSink::Counter refusedForeignGeneration { IMetricsSink::Counter::Last };
};

/// Whether @p profile states every column that has no absent reading.
///
/// **This is the missing-field guard.** Every such member defaults to its UNSTATED value -- an
/// unstated verb pair, `DropVerb::Unstated`, `Counter::Last` -- so a row that forgets one
/// compiles, and the missing-field warning no longer fires for it. **`LocalCache`'s constructors
/// refuse such a profile**, which is what makes the guard hold for a profile nobody wrote an assert
/// for: it is folded into the one operation every tier goes through, as `UpstreamCountingOf` is.
/// The `static_assert` beside each profile is the compile-time answer on top, not the guard. The
/// upstream members are not asked: their absence is a reading (*cannot happen here*), and
/// `UpstreamCountingOf` judges them.
///
/// The defaults exist because the analyser reads a member with none as a constructor that leaves
/// it uninitialized, in every translation unit where nothing copies a profile.
/// @param profile The row.
/// @return True when every such member is stated.
[[nodiscard]] constexpr bool StatesEveryMember(CacheTierProfile const& profile) noexcept
{
    auto const counters = std::array { profile.hits,
                                       profile.misses,
                                       profile.storeFailures,
                                       profile.refusedUnsupportedVersion,
                                       profile.refusedMalformedPayload,
                                       profile.refusedForeignGeneration };
    return profile.verbs.stated && profile.drop != DropVerb::Unstated
           && std::ranges::none_of(counters,
                                   [](IMetricsSink::Counter counter) { return counter == IMetricsSink::Counter::Last; });
}

/// How a profile counts reading through to an upstream. **PRIVATE: in-process only**, never
/// transmitted or persisted, so it carries no values.
enum class UpstreamCounting : std::uint8_t
{
    Every,   ///< All four upstream outcomes have a counter: a tier that reads through.
    None,    ///< None has: a tier that reads through to nothing.
    Partial, ///< Some have and some do not -- a profile that would count some outcomes of one upstream.
};

/// How @p profile counts reading through.
///
/// The question `LocalCache` asks of the pairing it is built with: a profile that names no
/// upstream counter describes a tier with nothing behind it, and paired with a real upstream every
/// read-through and every store it offered would move nothing an operator reads.
/// @param profile The profile.
/// @return Whether it counts every upstream outcome, none, or some.
[[nodiscard]] constexpr UpstreamCounting UpstreamCountingOf(CacheTierProfile const& profile) noexcept
{
    auto const members =
        std::array { profile.upstreamHits, profile.fillFailures, profile.upstreamStores, profile.upstreamStoreFailures };
    auto const counted = static_cast<std::size_t>(std::ranges::count_if(
        members, [](std::optional<IMetricsSink::Counter> const& member) { return member.has_value(); }));
    if (counted == members.size())
        return UpstreamCounting::Every;
    return counted == 0 ? UpstreamCounting::None : UpstreamCounting::Partial;
}

} // namespace FastCache::Node
