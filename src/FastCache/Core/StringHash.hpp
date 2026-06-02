// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace FastCache
{

/// Heterogeneous hasher for `std::unordered_map<std::string, ...>` keyed by
/// strings. Pairing it with `std::equal_to<>` lets `find`, `erase`, and
/// `count` accept a `std::string_view` (or `char const*`) directly, avoiding
/// the temporary `std::string` an ordinary `std::hash<std::string>` map would
/// force at every lookup.
///
/// All overloads hash through `std::hash<std::string_view>` so a key looked up
/// by view and the same key stored as `std::string` collide identically.
struct TransparentStringHash
{
    /// Opt the map into heterogeneous lookup.
    using is_transparent = void;

    /// @param sv Key view.
    /// @return Hash of the viewed bytes.
    [[nodiscard]] std::size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view> {}(sv);
    }

    /// @param s Owning key.
    /// @return Hash of the string's bytes.
    [[nodiscard]] std::size_t operator()(std::string const& s) const noexcept
    {
        return std::hash<std::string_view> {}(std::string_view { s });
    }

    /// @param s NUL-terminated key.
    /// @return Hash of the C-string's bytes.
    [[nodiscard]] std::size_t operator()(char const* s) const noexcept
    {
        return std::hash<std::string_view> {}(std::string_view { s });
    }
};

} // namespace FastCache
