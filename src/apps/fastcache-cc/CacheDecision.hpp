// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CacheProtocol.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace FastCache::Cc
{

/// What the launcher does with a FETCH, as a function of what came back.
///
/// The file is separate from `main.cpp` for the reason `HitVerification.hpp`,
/// `CacheProtocol.cpp` and `RootReconciler.cpp` are: **`main.cpp` is in no test
/// target** ([#370](https://github.com/LASTRADA-Software/fastcached/issues/370)), so
/// every decision made there is unreachable from the suite.
/// `src/tests/CMakeLists.txt` cites that fact in four separate places as the reason
/// something cannot be tested, which is the measure of how much has accumulated
/// behind it.
///
/// [#909](https://github.com/LASTRADA-Software/fastcached/issues/909) is the ticket,
/// and what makes it more than "some code is untested" is that **two shipping
/// blockers on one branch were both invisible for exactly this reason, and both were
/// caught by review rather than by a test.** Both are fixed; this is the half that
/// stops the next one being invisible too.
///
/// ## The one that costs every key in every fleet cache at once
///
/// An undecodable fetched value must be answered by **compiling and STORING OVER
/// IT**, never by compiling plainly. A `CompileValueVersion` bump does not move a
/// key -- only `objkey-v*` does, and a generation is deliberately not that -- so a
/// value of the previous generation sits under the exact key the new build computes.
/// Answer it without a store and it is fetched, refused, and never overwritten: on
/// every later build, for every key, forever. That is not the one cold cache a
/// generation bump promises but a **permanently dead** one, presenting as a slow
/// build with a `--show-stats` reason nobody reads.
///
/// The distance between the two behaviours is one `return` against one fall-through,
/// in a branch whose difference is invisible on any machine whose cache happens to
/// hold the current generation -- which is every developer machine, every time,
/// until the day of an upgrade.
///
/// ## Why an OBSERVATION enum rather than a struct of the raw facts
///
/// The obvious shape is `{ kind, isHit, decoded, disposition }` with three fields
/// documented as meaningful only when an earlier one holds. That is a state
/// collapse waiting to be read wrong: it can represent `decoded == true` on a miss,
/// and nothing would say which of the two the reader meant. The enumerators below
/// are the states the launcher can actually be in, so an impossible combination
/// cannot be spelled -- and `ObserveFetch` is the ONE place that folds the raw facts
/// into one, so there is a single line to get right rather than a branch per caller.

/// What was observed about a FETCH, as one state.
enum class FetchObservation : std::uint8_t
{
    /// The daemon never answered usefully -- refused the request, or the transport
    /// failed. Told apart elsewhere (an operator fixes them in different places),
    /// identical here: neither yields a value, and both leave this build to compile.
    NotServing,
    /// The daemon answered and held nothing under this key.
    Miss,
    /// A value came back that THIS build cannot decode. Overwhelmingly a generation
    /// this binary does not implement; also damaged bytes. Deliberately one state:
    /// the launcher's response is the same either way, and the two are told apart in
    /// the recorded reason rather than in the action.
    HitUndecodable,
    /// Decoded, materialized, and served. The only state in which nothing is
    /// compiled.
    HitServed,
    /// Decoded, but the object or depfile could not be written here. The cache is
    /// not usable on this machine right now.
    HitUnusable,
    /// Decoded, but a dependency it replays is missing here, so what it asserts is
    /// not true of this machine.
    HitStale,
    /// Not an observation, and has no row: the table's length.
    Last,
};

/// What the launcher does about it.
enum class CacheAction : std::uint8_t
{
    /// The hit stands. Nothing is compiled and nothing is stored.
    ServeFromCache,
    /// Compile for real, and STORE the result over this key.
    CompileAndStore,
    /// Compile for real and store nothing. Reserved for the case where the cache has
    /// shown itself unusable on this machine, where a store would fail the same way.
    CompileWithoutStoring,
    /// Not an action, and has no row: the table's length.
    Last,
};

/// One row per observation: what to do, and why -- the reason being the half a
/// reader of a failing test needs and the half that rots silently otherwise.
struct CacheDecisionRow
{
    FetchObservation observation; ///< The state this row is about.
    CacheAction action;           ///< What the launcher does in it.
    std::string_view why;         ///< Why, in the terms an operator would use.
};

/// The decision table, in enumerator order so an observation indexes its own row.
///
/// A table rather than a `switch` for the reason `DispositionTable` above it is one,
/// and with one extra force behind it here: a `switch` with a `default` accepts a new
/// enumerator silently and picks whatever the default arm does. On this enum the
/// default that reads as safest -- compile plainly -- is exactly the permanently-dead
/// -cache behaviour, so the arm a careless author would reach for is the defect.
/// `EnumTable` plus `RowsInEnumeratorOrder` makes an unhandled state fail to BUILD.
inline constexpr EnumTable<FetchObservation, CacheDecisionRow> CacheDecisionTable { {
    { .observation = FetchObservation::NotServing,
      .action = CacheAction::CompileAndStore,
      .why = "no answer worth reading; compile and offer the result to whoever is listening next" },
    { .observation = FetchObservation::Miss,
      .action = CacheAction::CompileAndStore,
      .why = "nothing under this key yet; this build is what puts it there" },
    { .observation = FetchObservation::HitUndecodable,
      .action = CacheAction::CompileAndStore,
      .why = "a value this build cannot read sits under a key a generation bump does not move, so the store is "
             "what repairs the entry rather than leaving it to poison every later build" },
    { .observation = FetchObservation::HitServed,
      .action = CacheAction::ServeFromCache,
      .why = "the hit held up and was written; nothing else runs" },
    { .observation = FetchObservation::HitUnusable,
      .action = CacheAction::CompileWithoutStoring,
      .why = "the object could not be written here, so the cache is not usable on this machine and a store would "
             "fail the same way" },
    { .observation = FetchObservation::HitStale,
      .action = CacheAction::CompileAndStore,
      .why = "the object is fine but its dependency record is not true here; the store overwrites this key with "
             "one that is" },
} };

static_assert(RowsInEnumeratorOrder(CacheDecisionTable, &CacheDecisionRow::observation),
              "CacheDecisionTable must hold one row per FetchObservation, in enumerator order -- the order is what "
              "lets an observation index its own row, and the completeness is what stops a new state defaulting to "
              "the permanently-dead-cache behaviour");

/// What the launcher does about an observed fetch.
/// @param observation The state the fetch ended in.
/// @return The action, read from the table.
[[nodiscard]] constexpr CacheAction DecideCacheAction(FetchObservation observation) noexcept
{
    return CacheDecisionTable[static_cast<std::size_t>(observation)].action;
}

/// Why, in the terms an operator would use.
/// @param observation The state the fetch ended in.
/// @return The reason for the action.
[[nodiscard]] constexpr std::string_view CacheActionReason(FetchObservation observation) noexcept
{
    return CacheDecisionTable[static_cast<std::size_t>(observation)].why;
}

/// Does this action compile the translation unit for real?
/// @param action The action.
/// @return True unless the hit was served.
[[nodiscard]] constexpr bool CompilesForReal(CacheAction action) noexcept
{
    return action != CacheAction::ServeFromCache;
}

/// Does this action STORE what the compile produced?
/// @param action The action.
/// @return True only for `CompileAndStore`.
[[nodiscard]] constexpr bool StoresResult(CacheAction action) noexcept
{
    return action == CacheAction::CompileAndStore;
}

/// What became of a cache hit we tried to honour.
///
/// Three outcomes rather than a bool, because the two failures want opposite
/// responses: a value whose dependency record no longer holds must be RECOMPILED AND
/// RE-STORED (which repairs the entry), while a value we simply could not write to
/// disk means the cache is not usable here and the compile should run plainly,
/// uncached.
///
/// It lived in `main.cpp` until #909, which put it out of reach of the suite along
/// with everything that read it.
enum class HitDisposition : std::uint8_t
{
    Served,   ///< Object and depfile written, streams replayed.
    Stale,    ///< A replayed dependency is missing here; recompile and re-store.
    Unusable, ///< The object or depfile could not be written; abandon the cache.
};

/// Fold what the fetch actually produced into one observation.
///
/// THE ONE PLACE the raw facts become a state, so the mapping is a single expression
/// rather than a branch at each caller. It is also the part a unit test cannot fully
/// stand in for -- `main.cpp` still has to call it with the right arguments -- which
/// is why it takes the narrowest possible inputs: a kind the protocol already
/// computes, and two facts with no reading required.
///
/// @param kind How the exchange ended, from `RunOneExchange`.
/// @param isHit Whether the answer carried a value.
/// @param decoded Whether that value decoded under this build's generation. Read
///        only when `isHit`; a miss carries no value to decode.
/// @param disposition What materializing it produced. Read only when `isHit` and
///        `decoded`.
/// @return The single state those facts describe.
[[nodiscard]] constexpr FetchObservation ObserveFetch(CacheOutcomeKind kind,
                                                      bool isHit,
                                                      bool decoded,
                                                      HitDisposition disposition) noexcept
{
    if (!CacheIsServing(kind))
        return FetchObservation::NotServing;
    if (!isHit)
        return FetchObservation::Miss;
    if (!decoded)
        return FetchObservation::HitUndecodable;
    switch (disposition)
    {
        case HitDisposition::Served:
            return FetchObservation::HitServed;
        case HitDisposition::Stale:
            return FetchObservation::HitStale;
        case HitDisposition::Unusable:
            return FetchObservation::HitUnusable;
    }
    // Unreachable for any value of the enum, and NOT a default arm: a default here
    // would silently absorb a fourth disposition into whatever it returned, which is
    // the failure this whole file exists to make impossible.
    return FetchObservation::HitUnusable;
}

} // namespace FastCache::Cc
