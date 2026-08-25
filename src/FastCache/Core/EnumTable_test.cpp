// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>
#include <type_traits>

using FastCache::EnumeratorCount;
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
