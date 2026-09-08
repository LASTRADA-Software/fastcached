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

/// Find the first element of @p range satisfying @p predicate, yielding a pointer.
///
/// Like `FindOrNull`, this exists for the **return type** rather than the
/// algorithm, and the reason is spelled out here rather than cross-referenced
/// because a reader arriving at the predicate form needs it *here*:
/// `std::ranges::find_if` is available on every toolchain the project builds
/// with, but the iterator it hands back cannot be named portably.
/// `std::array`'s iterator is a raw pointer in libstdc++ and libc++ and a class
/// type in the MSVC STL, so a call site that stores the result has no working
/// spelling — `auto const*` fails to compile on Windows with C3535, a plain
/// `auto const` trips clang-tidy's readability-qualified-auto, and writing the
/// type out trips its modernize-use-auto. Each of those has broken a build here.
///
/// Inside a template the iterator's type is dependent, so the conflict resolves
/// once, here, instead of at every lookup:
///
/// ```cpp
/// if (auto const* const row = FindIfOrNull(Table, [&](Row const& r) { return r.needle.contains(x); }))
///     return row->value;
/// ```
///
/// **Use this rather than forcing `FindOrNull`** with a `bool`-returning
/// projection and a `true` value. That spelling type-checks, and it inverts the
/// reading of every equality-on-a-member call site `FindOrNull` has.
///
/// No projection parameter, unlike `FindOrNull`: a predicate already subsumes
/// one, so a second customization point would be a spelling with no question of
/// its own to answer.
///
/// @param range Range to search. Borrowed, never copied.
/// @param predicate Applied to each element; the first `true` wins.
/// @return Pointer to the first match, or `nullptr` when nothing matches.
template <std::ranges::forward_range Range, typename Predicate>
[[nodiscard]] constexpr std::ranges::range_value_t<Range> const* FindIfOrNull(Range const& range, Predicate predicate)
{
    auto const it = std::ranges::find_if(range, std::move(predicate));
    return it != std::ranges::end(range) ? std::addressof(*it) : nullptr;
}

} // namespace FastCache
