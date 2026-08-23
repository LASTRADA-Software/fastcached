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
/// failure rather than a review comment. Guarding the dereference *here* is what
/// the check does understand, and the preceding `REQUIRE` still fails the test
/// first when the optional is empty, so the default is never actually observed.
///
/// It returns a **reference**, which is what finally settled an argument with
/// GCC. `value_or` was the first version and reads best; GCC 14 at `-O3` inlines
/// it and reports `-Wnull-dereference` inside `std::vector`'s copy constructor
/// for `T = std::vector<std::span<...>>`. A false positive, but `-Werror` does
/// not care, and it appears only in a release build on one compiler — no local
/// debug run and no sanitizer sees it. Writing the guard out as a ternary did not
/// help, because the copy is what the analysis objects to. Handing back a
/// reference removes the copy rather than arguing about it, and is what the
/// caller wanted anyway: every call site either compares the result or copies it
/// into a named value, and none of them needed a temporary in between.
///
/// Safe at every site because the argument is always a named optional or a
/// subobject of one, and the single case that passes a temporary
/// (`Unwrap(SplitFields(...))` in `CompileCacheWire_test`) binds the result to
/// `auto const`, which copies before the temporary dies. A future
/// `auto const& x = Unwrap(f())` would dangle — bind to `auto const` there.
///
/// One definition rather than the eleven near-identical copies this replaced.
/// They were near-identical rather than identical, which is the cost: each
/// carried its own abbreviation of the reasoning above, so the *reason* the idiom
/// exists was reconstructible from some copies and not from others — and a test
/// author who found one of the terse ones had nothing telling them why a plain
/// dereference was not simply better.
/// @tparam T The contained type.
/// @param value The optional to read.
/// @return A reference to its value, or to a shared default-constructed one.
template <typename T>
[[nodiscard]] T const& Unwrap(std::optional<T> const& value)
{
    // A single shared instance rather than a temporary, because a reference to a
    // temporary would dangle the moment this returns. It is never observed: the
    // `REQUIRE` that precedes every call fails the test first.
    static T const absent {};
    return value.has_value() ? *value : absent;
}

} // namespace FastCache::Testing
