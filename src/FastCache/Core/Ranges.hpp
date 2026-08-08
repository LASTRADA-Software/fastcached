// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>

namespace FastCache
{

/// Find the first element of @p range whose @p projection compares equal to
/// @p value, yielding a pointer.
///
/// This exists for the **return type**, not the algorithm: `std::ranges::find`
/// is available on every toolchain the project builds with, but the iterator it
/// hands back cannot be named portably. `std::array`'s iterator is a raw pointer
/// in libc++ and a class type in the MSVC STL, so a call site that stores the
/// result has no working spelling — `auto const*` fails to compile on Windows
/// with C3535, a plain `auto const` trips clang-tidy's
/// readability-qualified-auto, and writing the type out trips its
/// modernize-use-auto. Each of those has broken a build here.
///
/// Inside a template the iterator's type is dependent, so the conflict resolves
/// once, here, instead of at every lookup. Callers get a plain pointer, which
/// also gives "not found" its idiomatic spelling:
///
/// ```cpp
/// if (auto const* const row = FindOrNull(Table, key, &Row::key))
///     return row->value;
/// ```
///
/// Prefer this over `std::ranges::find` whenever the range may be a
/// `std::array` or a raw array; for `std::vector` and friends the iterator is a
/// class type everywhere and either form is portable.
///
/// @param range Range to search. Borrowed, never copied.
/// @param value Value each projected element is compared against.
/// @param projection Applied to an element before comparing; identity by default.
/// @return Pointer to the first match, or `nullptr` when nothing matches.
template <std::ranges::forward_range Range, typename Value, typename Projection = std::identity>
[[nodiscard]] constexpr std::ranges::range_value_t<Range> const* FindOrNull(Range const& range,
                                                                            Value const& value,
                                                                            Projection projection = {})
{
    auto const it = std::ranges::find(range, value, std::move(projection));
    return it != std::ranges::end(range) ? std::addressof(*it) : nullptr;
}

} // namespace FastCache
