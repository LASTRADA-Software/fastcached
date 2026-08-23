// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>

namespace FastCache::Testing
{

/// Unwrap an optional for assertion, yielding a default-constructed value when
/// empty.
///
/// clang-tidy's optional analysis cannot see a `has_value()` guard through
/// Catch2's `REQUIRE` macro, so a direct `*x` or `x.value()` after one is
/// reported as an unchecked access — and with `WarningsAsErrors` that is a build
/// failure rather than a review comment. Going through `value_or` is provably
/// safe, and the preceding `REQUIRE` still fails the test first when the optional
/// is empty, so the default is never actually observed.
///
/// One definition rather than the eleven near-identical copies this replaced.
/// They were near-identical rather than identical, which is the cost: each
/// carried its own abbreviation of the reasoning above, so the *reason* the idiom
/// exists was reconstructible from some copies and not from others — and a test
/// author who found one of the terse ones had nothing telling them why a plain
/// dereference was not simply better.
/// @tparam T The contained type.
/// @param value The optional to read.
/// @return Its value, or a default-constructed one.
template <typename T>
[[nodiscard]] T Unwrap(std::optional<T> const& value)
{
    return value.value_or(T {});
}

} // namespace FastCache::Testing
