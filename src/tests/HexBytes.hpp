// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace FastCache::Testing
{

/// The value of one hexadecimal digit.
/// @param digit `0-9`, `a-f` or `A-F`.
/// @return Its value, 0 to 15.
/// @throws std::invalid_argument for anything else, so a mistyped vector fails its case loudly
///         rather than decoding to bytes nobody published.
[[nodiscard]] inline unsigned HexDigitValue(char digit)
{
    constexpr std::string_view Lower = "0123456789abcdef";
    constexpr std::string_view Upper = "0123456789ABCDEF";
    if (auto const at = Lower.find(digit); at != std::string_view::npos)
        return static_cast<unsigned>(at);
    if (auto const at = Upper.find(digit); at != std::string_view::npos)
        return static_cast<unsigned>(at);
    throw std::invalid_argument(std::format("'{}' is not a hexadecimal digit", digit));
}

/// Decode hexadecimal text into bytes, the way RFC test vectors are written.
///
/// Strict, because the vectors it decodes are what a cryptographic test is checked AGAINST: an
/// odd length or a stray character is refused rather than skipped, since a lenient decoder turns a
/// typo into a different vector and a negative case into one that passes for the wrong reason.
/// @param hex An even number of hex digits, either case.
/// @return The bytes it spells.
/// @throws std::invalid_argument for an odd length or a character that is not a hex digit.
[[nodiscard]] inline std::vector<std::byte> FromHex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
        throw std::invalid_argument(std::format("{} hex digits cannot spell whole bytes", hex.size()));
    std::vector<std::byte> bytes;
    bytes.reserve(hex.size() / 2);
    for (auto const pair: std::views::iota(std::size_t { 0 }, hex.size() / 2))
        bytes.push_back(static_cast<std::byte>((HexDigitValue(hex[2 * pair]) << 4U) | HexDigitValue(hex[(2 * pair) + 1])));
    return bytes;
}

/// Decode hexadecimal text into a fixed-size array, for a key or signature type.
/// @tparam N The array's size in bytes; the text must spell exactly this many.
/// @param hex `2 * N` hex digits, either case.
/// @return The bytes it spells.
/// @throws std::invalid_argument when the text does not spell exactly @p N bytes.
template <std::size_t N>
[[nodiscard]] std::array<std::byte, N> ArrayFromHex(std::string_view hex)
{
    auto const bytes = FromHex(hex);
    if (bytes.size() != N)
        throw std::invalid_argument(std::format("expected {} bytes, the text spells {}", N, bytes.size()));
    std::array<std::byte, N> array {};
    std::ranges::copy(bytes, array.begin());
    return array;
}

} // namespace FastCache::Testing
