// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ranges.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>
#include <vector>

using FastCache::FindOrNull;

namespace
{
struct Row
{
    int key;
    std::string_view name;
};

constexpr auto Table = std::to_array<Row>({
    Row { .key = 1, .name = "one" },
    Row { .key = 2, .name = "two" },
    Row { .key = 3, .name = "two" }, // duplicate name on purpose; see "first match"
});
} // namespace

TEST_CASE("FindOrNull: locates an element by projection", "[core][ranges]")
{
    auto const* const row = FindOrNull(Table, 2, &Row::key);
    REQUIRE(row != nullptr);
    REQUIRE(row->name == "two");
}

TEST_CASE("FindOrNull: yields nullptr when nothing matches", "[core][ranges]")
{
    // The whole point of the pointer return: "absent" has an idiomatic spelling
    // that does not require naming an iterator type.
    REQUIRE(FindOrNull(Table, 99, &Row::key) == nullptr);
    REQUIRE(FindOrNull(Table, std::string_view { "nope" }, &Row::name) == nullptr);
}

TEST_CASE("FindOrNull: returns the first match, like std::ranges::find", "[core][ranges]")
{
    auto const* const row = FindOrNull(Table, std::string_view { "two" }, &Row::name);
    REQUIRE(row != nullptr);
    REQUIRE(row->key == 2);
}

TEST_CASE("FindOrNull: points into the range rather than at a copy", "[core][ranges]")
{
    // A copy would make the result useless for the lookup-then-read pattern the
    // helper exists for, and would dangle for a borrowed range.
    auto const* const row = FindOrNull(Table, 3, &Row::key);
    REQUIRE(row == &Table[2]);
}

TEST_CASE("FindOrNull: works without a projection", "[core][ranges]")
{
    constexpr auto Numbers = std::to_array({ 10, 20, 30 });
    REQUIRE(FindOrNull(Numbers, 20) == &Numbers[1]);
    REQUIRE(FindOrNull(Numbers, 40) == nullptr);
}

TEST_CASE("FindOrNull: handles containers whose iterator is a class type", "[core][ranges]")
{
    // std::array is the case that motivated the helper (raw-pointer iterator on
    // libc++, class type on the MSVC STL), but it must not be array-specific.
    std::vector<Row> const rows { Row { .key = 7, .name = "seven" } };
    auto const* const row = FindOrNull(rows, 7, &Row::key);
    REQUIRE(row != nullptr);
    REQUIRE(row->name == "seven");

    std::vector<Row> const empty;
    REQUIRE(FindOrNull(empty, 7, &Row::key) == nullptr);
}

TEST_CASE("FindOrNull: is usable in a constant expression", "[core][ranges]")
{
    // constexpr matters because TraitsOf in ServiceControl.cpp is constexpr;
    // a non-constexpr helper would have silently forced it to become runtime.
    static_assert(FindOrNull(Table, 1, &Row::key) != nullptr);
    static_assert(FindOrNull(Table, 42, &Row::key) == nullptr);
    static_assert(FindOrNull(Table, 3, &Row::key)->name == "two");
    SUCCEED();
}
