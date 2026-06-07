// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <span>
#include <string_view>

namespace CowTree
{

/// Read-only view over a contiguous byte sequence.
using BytesView = std::span<std::byte const>;

/// Mutable view over a contiguous byte sequence.
using BytesSpan = std::span<std::byte>;

/// Construct a BytesView from a std::string_view without copying.
/// @param sv Source view; lifetime must outlive the returned BytesView.
/// @return Span over the same bytes.
/// @note Not `constexpr`: the `reinterpret_cast` between `char` and `std::byte`
///       pointers is never valid in a constant expression, which Apple clang
///       diagnoses as `-Winvalid-constexpr`.
[[nodiscard]] inline BytesView AsBytes(std::string_view sv) noexcept
{
    return BytesView { reinterpret_cast<std::byte const*>(sv.data()), sv.size() };
}

/// Construct a std::string_view from a BytesView without copying.
/// @param bv Source view; lifetime must outlive the returned string_view.
/// @return string_view over the same bytes.
/// @note Not `constexpr` — see AsBytes.
[[nodiscard]] inline std::string_view AsStringView(BytesView bv) noexcept
{
    return std::string_view { reinterpret_cast<char const*>(bv.data()), bv.size() };
}

} // namespace CowTree
