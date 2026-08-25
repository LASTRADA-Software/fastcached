// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <ranges>
#include <type_traits>

namespace FastCache
{

/// An enum that states how many enumerators it has, with a trailing `Last`.
///
/// `Last` is not one of the things the enum names -- it is the count, and every
/// table below is sized from it. An enum without one cannot be the subject of a
/// total completeness check at all: C++ has no way to enumerate an enum's
/// enumerators, so the count has to be written down somewhere, and writing it
/// down inside the enum is the only spelling that cannot go stale when an
/// enumerator is appended.
template <typename Enum>
concept EnumWithLast = std::is_enum_v<Enum> && requires { Enum::Last; };

/// How many enumerators @p Enum has, not counting `Last` itself.
template <EnumWithLast Enum>
inline constexpr std::size_t EnumeratorCount = static_cast<std::size_t>(Enum::Last);

/// A table holding exactly one row per enumerator of @p Enum, indexable by it.
///
/// The extent comes from the enum rather than from a literal or from the
/// initializer, so appending an enumerator grows the array and leaves a
/// value-initialized row behind -- which `RowsInEnumeratorOrder` then rejects,
/// because a zeroed row claims to describe enumerator 0 and does not sit there.
/// Declaring the table any other way is what lets a short table through.
template <EnumWithLast Enum, typename Row>
using EnumTable = std::array<Row, EnumeratorCount<Enum>>;

/// Whether every row sits at the index of the enumerator it describes.
///
/// This is the whole of the rule that makes indexing a table by its enumerator
/// safe, and it is deliberately one function rather than one per table. It used
/// to be written out at each site, five different ways, and four of those
/// anchored the length on an enumerator **by name**:
///
/// ```cpp
/// static_assert(Table.size() == static_cast<std::size_t>(Enum::TheLastOneToday) + 1);
/// ```
///
/// That guard is inverted. Append an enumerator and forget the row, and
/// `size()` still equals `TheLastOneToday + 1`, so it compiles and then indexes
/// past the end; append one and *remember* the row, and the size is now one
/// greater and the assert fails. It fires only when nothing is wrong, and is
/// silent when something is -- which is the one shape a guard must not have.
///
/// @p Enum is deduced from what @p project returns, so a call site names no
/// template argument. Both halves of the rule are checked: the table's extent
/// against the enum's own count, and each row's position.
///
/// @param table The table to check. Any `std::array`; the extent is verified.
/// @param project Applied to a row to recover the enumerator it describes.
/// @return True when the table covers the enum, one row each, in order.
template <typename Row, std::size_t N, typename Projection>
[[nodiscard]] consteval bool RowsInEnumeratorOrder(std::array<Row, N> const& table, Projection project) noexcept
{
    using Enum = std::remove_cvref_t<std::invoke_result_t<Projection, Row const&>>;
    static_assert(EnumWithLast<Enum>,
                  "the projection must yield an enum carrying a trailing `Last` -- without one there is no count to "
                  "check the table's length against");
    static_assert(N == EnumeratorCount<Enum>,
                  "the table must hold exactly one row per enumerator; declare it as EnumTable<Enum, Row> so its "
                  "length comes from the enum rather than from however many rows somebody last wrote");

    return std::ranges::all_of(std::views::iota(std::size_t { 0 }, N), [&table, &project](std::size_t index) {
        return static_cast<std::size_t>(std::invoke(project, table[index])) == index;
    });
}

} // namespace FastCache
