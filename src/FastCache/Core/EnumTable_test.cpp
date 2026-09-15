// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>

using FastCache::EnumeratorCount;
using FastCache::Enumerators;
using FastCache::EnumTable;
using FastCache::EnumWithLast;
using FastCache::RowsInEnumeratorOrder;

namespace
{
enum class Colour : std::uint8_t
{
    Red = 0,
    Green,
    Blue,
    Last,
};

enum class Unbounded : std::uint8_t
{
    One = 0,
    Two,
};

struct ColourRow
{
    std::string_view name; ///< Deliberately first: the enumerator is not field zero.
    Colour colour;
};

constexpr EnumTable<Colour, ColourRow> WellFormed { {
    { .name = "red", .colour = Colour::Red },
    { .name = "green", .colour = Colour::Green },
    { .name = "blue", .colour = Colour::Blue },
} };

/// The same rows with two transposed -- the mistake a table of near-identical
/// rows actually invites, as opposed to a row being missing outright.
constexpr EnumTable<Colour, ColourRow> Transposed { {
    { .name = "red", .colour = Colour::Red },
    { .name = "blue", .colour = Colour::Blue },
    { .name = "green", .colour = Colour::Green },
} };

} // namespace

TEST_CASE("EnumWithLast: an enum qualifies only when it states its own count", "[core][enumtable]")
{
    STATIC_REQUIRE(EnumWithLast<Colour>);
    STATIC_REQUIRE_FALSE(EnumWithLast<Unbounded>);
    STATIC_REQUIRE_FALSE(EnumWithLast<int>);
}

TEST_CASE("Enumerators: every enumerator, in declaration order, and no Last", "[core][enumtable]")
{
    // WHAT DISTINGUISHES: the ORDER and the EXTENT together. A view yielding the right
    // COUNT of the wrong values passes any size assertion, and one yielding the right
    // values in the wrong order passes any set assertion -- so the values are checked
    // against their positions, which only one answer satisfies.
    constexpr auto Walked = [] {
        std::array<Colour, EnumeratorCount<Colour>> seen {};
        std::size_t at = 0;
        for (auto const colour: Enumerators<Colour>())
        {
            if (at >= seen.size())
                return std::pair { seen, std::size_t { 0 } - 1 }; // more values than the count: fail loudly
            seen[at++] = colour;
        }
        return std::pair { seen, at };
    }();
    STATIC_REQUIRE(Walked.second == EnumeratorCount<Colour>);
    STATIC_REQUIRE(Walked.first[0] == Colour::Red);
    STATIC_REQUIRE(Walked.first[1] == Colour::Green);
    STATIC_REQUIRE(Walked.first[2] == Colour::Blue);

    // And `Last` is not one of them. Asserted on the VALUE rather than on the count,
    // because a walk that stopped one early would satisfy the count check above only by
    // also dropping `Blue`, which the position checks catch -- these are the two halves of
    // one claim and neither covers the other.
    STATIC_REQUIRE(std::ranges::none_of(Enumerators<Colour>(), [](Colour c) { return c == Colour::Last; }));
    STATIC_REQUIRE(std::ranges::size(Enumerators<Colour>()) == 3);
}

TEST_CASE("Enumerators: from an enumerator, and empty past the end", "[core][enumtable]")
{
    STATIC_REQUIRE(std::ranges::size(Enumerators(Colour::Green)) == 2);
    // NAMED, because `ranges::begin` refuses an rvalue that is not a borrowed range and a
    // `transform_view` is not one: the temporary spelling does not compile, which is the
    // library saying the iterator would outlive its range.
    STATIC_REQUIRE([] {
        auto range = Enumerators(Colour::Green);
        return *range.begin();
    }() == Colour::Green);

    // `Last` yields nothing, and so does anything past it: the sub-range clamps rather
    // than walking backwards, which an unclamped `iota(first, count)` would do by
    // producing a reversed empty range on one implementation and undefined behaviour on
    // another. Both directions, because a clamp that only ever sees legal inputs is
    // untested rather than proven.
    STATIC_REQUIRE(std::ranges::empty(Enumerators(Colour::Last)));
    STATIC_REQUIRE(std::ranges::empty(Enumerators(static_cast<Colour>(9))));
    STATIC_REQUIRE(std::ranges::size(Enumerators(Colour::Red)) == EnumeratorCount<Colour>);
}

TEST_CASE("EnumeratorCount: counts the named enumerators, not Last", "[core][enumtable]")
{
    STATIC_REQUIRE(EnumeratorCount<Colour> == 3);
}

TEST_CASE("EnumTable: takes its length from the enum rather than the initializer", "[core][enumtable]")
{
    // This is where the length half of the rule is *enforced*: the extent is the
    // enum's own count whatever the initializer says, so a table one row short
    // cannot be declared -- it is a table with a value-initialized row, which the
    // last case below shows the order check rejecting.
    //
    // `RowsInEnumeratorOrder` re-checks the extent for a table declared as a plain
    // `std::array`, and that half has no case here on purpose: it is a
    // `static_assert`, so it is a hard error at instantiation rather than a
    // substitution failure, and nothing in the language can observe it. Verified
    // by hand instead -- a `std::array<ColourRow, 2>` passed here reports "the
    // table must hold exactly one row per enumerator".
    STATIC_REQUIRE(WellFormed.size() == EnumeratorCount<Colour>);
    STATIC_REQUIRE(std::is_same_v<decltype(WellFormed), EnumTable<Colour, ColourRow> const>);
}

TEST_CASE("RowsInEnumeratorOrder: accepts one row per enumerator, in order", "[core][enumtable]")
{
    STATIC_REQUIRE(RowsInEnumeratorOrder(WellFormed, &ColourRow::colour));
}

TEST_CASE("RowsInEnumeratorOrder: rejects two rows swapped", "[core][enumtable]")
{
    // Both rows are present and both name a real enumerator, so a length check
    // alone passes this -- which is why the order half is not redundant.
    STATIC_REQUIRE(Transposed.size() == EnumeratorCount<Colour>);
    STATIC_REQUIRE_FALSE(RowsInEnumeratorOrder(Transposed, &ColourRow::colour));
}

TEST_CASE("RowsInEnumeratorOrder: a value-initialized trailing row does not pass", "[core][enumtable]")
{
    // What an appended enumerator actually leaves behind: `EnumTable` grows, the
    // new row is zeroed, and a zeroed row claims enumerator 0 while sitting at
    // the end. This is the mechanism by which forgetting a row fails the build.
    constexpr EnumTable<Colour, ColourRow> missingLastRow { {
        { .name = "red", .colour = Colour::Red },
        { .name = "green", .colour = Colour::Green },
    } };

    STATIC_REQUIRE(missingLastRow.size() == EnumeratorCount<Colour>);
    STATIC_REQUIRE(missingLastRow[2].colour == Colour::Red);
    STATIC_REQUIRE_FALSE(RowsInEnumeratorOrder(missingLastRow, &ColourRow::colour));
}
