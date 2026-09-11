// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>

namespace FastCache::Testing
{

/// One generation of a versioned digest contract: what NAMED it, and the digest
/// taken under it.
///
/// Two tables in this tree keep this shape and kept it separately — the key space's
/// schema tags (`apps/fastcache-cc/`, keyed on a string) and the stored-value
/// contract's generations (`CompileCache/`, keyed on a `CompileValueVersion` byte).
/// They could not share a definition because **a library test may not include an app
/// header**, which is the same constraint that put `FleetHarness.hpp` here rather
/// than beside `RaftClusterHarness` (#548). Hence `src/tests/`, and hence the key
/// being a template parameter rather than one of the two spellings winning.
///
/// The cost of two copies is the one `.agent/rules/testing.md` already records for a
/// test fake: both answer *did somebody change behaviour without moving the tag*, and
/// if they diverge in HOW STRICTLY they answer it, one contract gets a real guard and
/// the other a decorative one with nothing to reveal the difference.
///
/// @tparam Key What names a generation — a schema tag, a version byte.
template <typename Key>
struct Generation
{
    Key key;                 ///< What this generation was written under.
    std::string_view digest; ///< What the CURRENT construction yields under that key.
};

/// Render a generation key for a failure message.
///
/// **A `std::uint8_t` streams as a CHARACTER**, so generation 4 would reach the
/// reader as an unprintable byte and generation 49 as `'1'`. The byte-keyed caller
/// spelled `static_cast<unsigned>` at each of its four message sites and the
/// string-keyed one needed no cast at all, which is precisely the divergence this
/// consolidation is for: a shared helper that took the key raw would have been
/// correct for one caller and quietly useless for the other, in the message somebody
/// reads exactly once, while it was failing.
///
/// @param key The generation key.
/// @return The key, promoted to a printable integer where it is integral.
template <typename Key>
[[nodiscard]] auto RenderGenerationKey(Key const& key)
{
    if constexpr (std::is_integral_v<Key>)
        return static_cast<std::uintmax_t>(key);
    else
        return key;
}

/// Require that the table has rows at all.
///
/// Shared by every assertion below and stated once, because an emptied table passes
/// all of them vacuously and reads exactly like one that found nothing wrong.
///
/// @param rows The generation table.
template <typename Key>
void RequireGenerationsPopulated(std::span<Generation<Key> const> rows)
{
    INFO("the generation table is empty, so every check over it passes vacuously");
    REQUIRE_FALSE(rows.empty());
}

/// Require that a live digest reproduces no RETIRED generation's digest.
///
/// **This is the guard for a table whose digested inputs are FROZEN, and it is
/// vacuous for one whose inputs grow.** That distinction is the whole reason the two
/// callers here assert differently, and it was invisible while each kept its own copy:
///
///   * where the inputs are pinned in the case (the key space's `KeyInputs`), putting
///     an old tag back re-computes that generation's digest exactly, so the live value
///     collides with a retired row and this fails. A golden vector alone cannot catch
///     that — the tag and the vector are one edit two hunks apart, and moving both
///     back leaves the suite green;
///   * where the inputs GROW (the stored-value conformance corpus, which gained three
///     rows in the same commit that retired generation 1), a retired digest describes
///     the corpus as that generation met it and nothing can re-derive it. The live
///     value then differs from it for a reason that has nothing to do with the bump,
///     and this check cannot fail. Use the structural pair below instead, and see
///     [#583](https://github.com/LASTRADA-Software/fastcached/issues/583) for what it
///     would take to make a retired row mean something again.
///
/// A table of retired rows only: the live generation is not among them, by
/// construction, since the live construction must not reproduce any row.
///
/// @param live The digest computed now, over the same pinned inputs the rows cover.
/// @param retired Every generation this contract has retired.
template <typename Key>
void RequireNoRetiredDigest(std::string_view live, std::span<Generation<Key> const> retired)
{
    RequireGenerationsPopulated(retired);
    for (auto const& generation: retired)
    {
        INFO("the live digest reproduces retired generation " << RenderGenerationKey(generation.key)
                                                              << " -- its bump was reverted");
        CHECK(live != generation.digest);
    }
}

/// Require that generations are unique and ascending.
///
/// The structural half, for a table that CONTAINS its live row. A bump appends, so
/// order is what a reverting author has to defeat — and it is what still bites when
/// `RequireNoRetiredDigest` cannot, because it says nothing about digests at all.
///
/// @param rows The generation table, oldest first.
template <typename Key>
void RequireGenerationsAscending(std::span<Generation<Key> const> rows)
{
    RequireGenerationsPopulated(rows);
    for (auto const i: std::views::iota(std::size_t { 1 }, rows.size()))
    {
        INFO("the generation table is out of order at row " << i << ": generations are unique and ascending, so a "
                                                            << "bump appends. Row " << i << " is "
                                                            << RenderGenerationKey(rows[i].key) << " after "
                                                            << RenderGenerationKey(rows[i - 1].key));
        // Promoted on BOTH sides, because Catch2 stringifies the operands itself and
        // never sees the note above: with a raw `std::uint8_t` the expansion line reads
        // `with expansion:` and then nothing at all, generation 4 being an unprintable
        // byte. Measured, watching this very assertion refuse.
        REQUIRE(RenderGenerationKey(rows[i].key) > RenderGenerationKey(rows[i - 1].key));
    }
}

/// Require that the live generation names the LAST row.
///
/// The other half of the structural pair: a bump appends a row, so putting the key
/// back names an earlier row however good the digest pasted beside it is.
///
/// @param live The generation this build declares itself to be.
/// @param rows The generation table, oldest first.
template <typename Key>
void RequireLiveGenerationIsLast(Key live, std::span<Generation<Key> const> rows)
{
    RequireGenerationsPopulated(rows);
    INFO("this build declares generation "
         << RenderGenerationKey(live) << " but the newest row is " << RenderGenerationKey(rows.back().key)
         << ". A bump appends a row, so the live key is the last one -- naming an earlier generation is a bump that "
            "was reverted, whatever digest was pasted with it.");
    // Promoted for the same reason as the ascending check's operands: the expansion
    // is Catch2's own stringification, not this header's stream.
    CHECK(RenderGenerationKey(live) == RenderGenerationKey(rows.back().key));
}

} // namespace FastCache::Testing
