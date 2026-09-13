// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace FastCache::Cli::Testing
{

/// @file ScriptedCellWidth.hpp
/// A cell-width function for tests that knows exactly the characters the tests use.
///
/// **Not the width function, and deliberately not a general one.** Production measures text
/// through the vendored terminal library, reached by the adapter layer; a panel test must not
/// depend on that library's tables, and must still put WIDE and ZERO-width text through the layout
/// -- a fixture of ASCII alone passes under a layout that counts bytes. So this names the ranges the
/// tests draw from: CJK and fullwidth forms are two cells, combining diacritics are none, and
/// everything else is one. A test that needs another range adds a row here.

/// A range of code points that all take the same number of cells.
struct CellRange
{
    char32_t first;    ///< The first code point in the range.
    char32_t last;     ///< The last code point in the range.
    std::size_t cells; ///< How many cells each takes.
};

/// The ranges this fake knows that are not one cell.
inline constexpr auto FakeCellRanges = std::to_array<CellRange>({
    { .first = U'\u0300', .last = U'\u036F', .cells = 0 }, // combining diacritical marks
    { .first = U'\u4E00', .last = U'\u9FFF', .cells = 2 }, // CJK unified ideographs
    { .first = U'\uFF01', .last = U'\uFF60', .cells = 2 }, // fullwidth forms
});

/// How many bytes the UTF-8 sequence led by @p lead takes.
/// @param lead The sequence's first byte.
/// @return One to four.
[[nodiscard]] constexpr unsigned SequenceLength(unsigned char lead) noexcept
{
    if (lead < 0x80U)
        return 1U;
    if (lead < 0xE0U)
        return 2U;
    if (lead < 0xF0U)
        return 3U;
    return 4U;
}

/// How many cells @p text occupies, by `FakeCellRanges`.
/// @param text Valid UTF-8.
/// @return The width in cells.
[[nodiscard]] inline std::size_t FakeCellWidth(std::string_view text) noexcept
{
    auto cells = std::size_t { 0 };
    auto at = std::size_t { 0 };
    while (at < text.size())
    {
        auto const lead = static_cast<unsigned char>(text[at]);
        auto const length = SequenceLength(lead);
        auto codePoint = static_cast<char32_t>(length == 1 ? lead : lead & (0xFFU >> (length + 1)));
        for (auto const offset: { std::size_t { 1 }, std::size_t { 2 }, std::size_t { 3 } })
            if (offset < length && at + offset < text.size())
                codePoint = (codePoint << 6U) | (static_cast<unsigned char>(text[at + offset]) & 0x3FU);
        auto width = std::size_t { 1 };
        for (auto const& range: FakeCellRanges)
            if (codePoint >= range.first && codePoint <= range.last)
                width = range.cells;
        cells += width;
        at += length;
    }
    return cells;
}

} // namespace FastCache::Cli::Testing
