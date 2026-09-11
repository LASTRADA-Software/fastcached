// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

/// Is this text a digest of the construction these tables carry?
///
/// Lowercase hex and nothing else, because that is what `HexDigest` emits. Accepting
/// uppercase would accept a value hand-typed rather than computed, which is precisely
/// the row `RequireDigestsComparable` exists to refuse.
///
/// @param text The candidate digest.
/// @return Whether it is non-empty lowercase hex.
[[nodiscard]] inline bool IsHexDigest(std::string_view text)
{
    return !text.empty() && std::ranges::all_of(text, [](char const c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

/// Require that a live digest and its table can meaningfully be compared at all.
///
/// **Every check below concludes from an INEQUALITY, so anything that differs from
/// every row passes** — and an empty or malformed `live` differs from every row. The
/// helper would then answer *no retired generation is reachable* for a digest it was
/// never handed: a guard that cannot fail in its accepting direction, inside the
/// header consolidated to make exactly that checkable (#548). It was reachable:
/// `RequireNoRetiredDigest("", rows)` passed every row.
///
/// The same inequality covers a ROW that no build could have produced. A row pasted
/// from the wrong place forbids nothing while the table still reports itself
/// populated, which `CacheKey_test.cpp`'s own step 2 has warned about in prose since
/// the table was written, checked by nothing. Width is derived from `live` rather
/// than pinned, because the two callers digest differently — 32 hex characters on the
/// key side, 64 on the stored-value side — and a constant here would be a third
/// spelling of a fact the data already carries.
///
/// Rows must also be pairwise DISTINCT: two rows carrying one digest is one
/// generation guarded twice and another not guarded at all, and it reads in a diff as
/// two generations covered.
///
/// This is deliberately NOT asked of the structural pair below. That table's retired
/// rows are DATED RECORDS whose digests no build can reproduce — that being what a
/// dated record is — so requiring re-derivability there would refuse a correct tree.
///
/// @param live The digest computed now.
/// @param rows The generation table it will be compared against.
template <typename Key>
void RequireDigestsComparable(std::string_view live, std::span<Generation<Key> const> rows)
{
    RequireGenerationsPopulated(rows);
    {
        INFO("the live digest is '"
             << live
             << "', which is not one this tree produces. Every check here concludes from an INEQUALITY, so "
                "a value that is empty or malformed differs from every row and the table reports no retired "
                "generation reachable for a digest nothing computed.");
        REQUIRE(IsHexDigest(live));
    }
    for (auto const& row: rows)
    {
        INFO("retired generation " << RenderGenerationKey(row.key) << " carries '" << row.digest
                                   << "', which is not a digest of the same construction as the live one ("
                                   << live.size()
                                   << " characters). Such a row forbids nothing while the table still reports "
                                      "itself populated.");
        REQUIRE(IsHexDigest(row.digest));
        REQUIRE(row.digest.size() == live.size());
    }
    for (auto const i: std::views::iota(std::size_t { 1 }, rows.size()))
        for (auto const j: std::views::iota(std::size_t { 0 }, i))
        {
            INFO("retired generations " << RenderGenerationKey(rows[j].key) << " and "
                                        << RenderGenerationKey(rows[i].key)
                                        << " carry the SAME digest, so one comparison covers both and one of "
                                           "them is guarded by nothing.");
            REQUIRE(rows[i].digest != rows[j].digest);
        }
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
    RequireDigestsComparable(live, retired);
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
/// **`RequireDigestsComparable` deliberately does not reach this pair, and the
/// asymmetry is a decision rather than an oversight.** A table keeping its retired
/// rows holds DATED RECORDS: a digest describing the corpus as that generation met
/// it, which nothing can recompute once the corpus or the behaviour has moved
/// ([#583](https://github.com/LASTRADA-Software/fastcached/issues/583)). Demanding
/// those digests be re-derivable is demanding they stop being dated records, and it
/// would refuse a correct tree. That is worth saying HERE rather than only at the
/// precondition, because an inconsistency between two tables in one header reads as
/// something to tidy up, and the same check in the wrong place turns a documented
/// decision into a red. These two functions compare KEYS, which are re-derivable by
/// construction; the digest half compares against a value computed NOW, which is
/// what makes a precondition meaningful there and vacuous here.
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
