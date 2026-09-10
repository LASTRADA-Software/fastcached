// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <string_view>

namespace FastCache::Cc
{

/// Whether a refusal row states exactly one of the two claims a refusal can make.
///
/// `Refuse` says a rise means something an operator acts on; `RefuseWithoutCounter`
/// says a rise would mean nothing, and why. A row must assert one of those and not the
/// other: a row asserting NEITHER is what a guard short-circuiting on the absent counter
/// passes vacuously, shipping a new refusal uncounted and unexplained, and a row
/// asserting BOTH is an author who could not choose, answered here rather than at
/// whichever call site read the fields in the luckier order.
///
/// **In `Protocol/` rather than in the node's endpoint header** (#640).
/// `SurfaceRefusal.hpp` argues that refusal coverage is a property of the TYPE, and the
/// rule this encodes is the metrics rulebook's -- *a refusal's wire code and its counter
/// are one row* -- which is not node-specific: any surface that refuses owes it. It was
/// hoisted into `apps/fastcache-compile-node/FrameEndpoint.hpp` during #523 because
/// `Protocol/` was outside that lane's grant, which left the next surface to re-derive it
/// or do without. It briefly WAS three: one named predicate for the cache plus the same
/// truth table open-coded twice as `answer.has_value() != !rationale.empty()`, two of them
/// in the inverted form and neither reachable by a grep for the name.
///
/// **In its own header rather than in `SurfaceRefusal.hpp`, which is where #640 expected
/// it, and the reason is a constraint that ticket told me to check for.**
/// `check-worker-refusals-counted.cmake` DERIVES the set of refusal spellings from
/// `SurfaceRefusal.hpp`, by taking every `[[nodiscard]]` declaration in it -- which is
/// what stops a fourth spelling being added there and asserting nothing -- and then
/// requires that set to agree with the number of `EncodeErrorReply` calls, because "each
/// spelling is built on exactly one". Putting these two functions there made it **5
/// spellings against 3 encoder calls**, and the check refused, correctly: by its
/// definition a `[[nodiscard]]` function in that file IS a way to answer a refusal, and
/// these are not. Relaxing the derivation to admit them would trade an exact guard for a
/// refactor's convenience. A sibling header, included by `SurfaceRefusal.hpp` so every
/// consumer still gets it for free, keeps both properties.
///
/// @param counted Whether the row carries a counted answer.
/// @param rationale The row's reason for counting nothing; empty when it counts.
/// @return True when exactly one of the two is present.
[[nodiscard]] constexpr bool StatesOneRefusalClaim(bool counted, std::string_view rationale) noexcept
{
    return counted == rationale.empty();
}

/// What a refusal row asserts, in the one vocabulary all three surfaces share.
///
/// A view rather than a field on any row type: the three tables spell the claim
/// differently on purpose -- two carry it flat as `answer`/`rationale`, the cache's
/// carries a nested `policy` -- and asking each to adopt a common layout would be a
/// change to three surfaces for one assertion's convenience.
struct RefusalClaim
{
    bool counted;               ///< Whether the row carries a counted answer.
    std::string_view rationale; ///< Why nothing is counted; empty when it counts.
};

/// Whether every row of a refusal table states one claim.
///
/// The table-level form, so the three surfaces share the ASSERTION and not only the
/// predicate. Each spelled its own `std::ranges::all_of` over its own row type: the
/// predicate inside them was already one function and the loop around it was three,
/// which is the shape a fourth surface copies rather than the one it calls.
///
/// The projection stays at the call site because that is the part that genuinely
/// differs, and naming its result `RefusalClaim` is what keeps three spellings of one
/// idea from being three ideas.
///
/// @tparam Table Anything `std::ranges::all_of` accepts.
/// @tparam Project Callable from a row to a `RefusalClaim`.
/// @param table The rows to check.
/// @param claimOf How this table's row spells its claim.
/// @return True when every row states exactly one claim.
template <typename Table, typename Project>
[[nodiscard]] constexpr bool RowsStateOneRefusalClaim(Table const& table, Project claimOf) noexcept
{
    return std::ranges::all_of(table, [&claimOf](auto const& row) {
        RefusalClaim const claim = claimOf(row);
        return StatesOneRefusalClaim(claim.counted, claim.rationale);
    });
}

} // namespace FastCache::Cc
