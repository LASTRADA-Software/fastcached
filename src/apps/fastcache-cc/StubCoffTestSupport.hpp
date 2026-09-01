// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc::Test
{

/// Building COFF object images for tests, in ONE place.
///
/// Synthetic rather than compiled, because most of what has to be asserted about
/// object equivalence cannot be produced on demand by a compiler: an object whose
/// `.text$mn` differs while everything else matches is what a WRONG object looks
/// like, and no arrangement of flags makes `cl` emit one.
///
/// Shared rather than written per test file for the reason
/// `StubObjectTestSupport.hpp` and `src/tests/ScriptedSocket.hpp` both record: two
/// hand-rolled copies of one layout had already diverged -- one of them set
/// `PointerToSymbolTable` to zero and so exercised the other arm of the consistency
/// check than its own test name claimed -- and one of them hand-recomputed the
/// header and section-header sizes a third time, in a constant meant to point at
/// section DATA. When a layout constant moves, a private copy stops testing what it
/// says it tests and reports nothing.

/// One section of a stub object.
struct StubSection
{
    std::string name;
    std::string data;
};

/// Which of the two COFF header layouts a stub is built to.
enum class StubLayout : std::uint8_t
{
    /// The ordinary `IMAGE_FILE_HEADER`.
    Standard,
    /// `ANON_OBJECT_HEADER_BIGOBJ`, which `cl /bigobj` writes -- a different
    /// structure, whose clock is at offset 8 and whose section count is 32 bits.
    BigObj,
};

/// Bytes the fixed header occupies, and therefore where the section table starts.
/// @param layout Which layout.
/// @return The size.
[[nodiscard]] constexpr std::size_t StubHeaderSize(StubLayout layout) noexcept
{
    return layout == StubLayout::BigObj ? 56U : 20U;
}

/// A COFF section header is forty bytes in both layouts.
inline constexpr std::size_t StubSectionHeaderSize = 40;

/// How many symbol records a stub carries.
inline constexpr std::size_t StubSymbolCount = 2;

/// Write @p value into @p bytes at @p at, little-endian, over @p width bytes.
/// @param bytes The image being built.
/// @param at Where the field starts.
/// @param width How wide it is.
/// @param value What to store.
inline void PutLe(std::vector<std::byte>& bytes, std::size_t at, std::size_t width, std::uint64_t value)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, width))
        bytes[at + index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
}

/// Build a structurally valid COFF object.
///
/// @param sections The sections, in file order.
/// @param timestamp What to put in `TimeDateStamp`.
/// @param layout Which header layout to write.
/// @param version The `/bigobj` version field; ignored for the standard layout.
/// @return The image.
[[nodiscard]] inline std::vector<std::byte> BuildCoff(std::vector<StubSection> const& sections,
                                                      std::uint32_t timestamp,
                                                      StubLayout layout = StubLayout::Standard,
                                                      std::uint16_t version = 2)
{
    auto const big = layout == StubLayout::BigObj;
    auto const tableAt = StubHeaderSize(layout);
    auto const symbolRecord = big ? std::size_t { 20 } : std::size_t { 18 };
    auto const dataAt = tableAt + (StubSectionHeaderSize * sections.size());

    std::size_t payload = 0;
    for (auto const& section: sections)
        payload += section.data.size();

    auto const symbolsAt = dataAt + payload;
    auto const stringsAt = symbolsAt + (symbolRecord * StubSymbolCount);
    // The string table opens with its own size, INCLUDING those four bytes, and is
    // the last thing in the file -- which is what makes a truncated object detectable.
    std::vector<std::byte> image(stringsAt + 4, std::byte { 0 });

    if (big)
    {
        PutLe(image, 0, 2, 0);
        PutLe(image, 2, 2, 0xFFFF);
        PutLe(image, 4, 2, version);
        PutLe(image, 6, 2, 0x8664);
        PutLe(image, 8, 4, timestamp);
        PutLe(image, 44, 4, sections.size());
        PutLe(image, 48, 4, symbolsAt);
        PutLe(image, 52, 4, StubSymbolCount);
    }
    else
    {
        PutLe(image, 0, 2, 0x8664);
        PutLe(image, 2, 2, sections.size());
        PutLe(image, 4, 4, timestamp);
        PutLe(image, 8, 4, symbolsAt);
        PutLe(image, 12, 4, StubSymbolCount);
        PutLe(image, 16, 2, 0);
        PutLe(image, 18, 2, 0);
    }

    auto cursor = dataAt;
    for (auto const index: std::views::iota(std::size_t { 0 }, sections.size()))
    {
        auto const at = tableAt + (StubSectionHeaderSize * index);
        auto const& section = sections[index];
        std::ranges::transform(section.name | std::views::take(8),
                               std::next(image.begin(), static_cast<std::ptrdiff_t>(at)),
                               [](char c) { return static_cast<std::byte>(c); });
        PutLe(image, at + 16, 4, section.data.size());
        PutLe(image, at + 20, 4, section.data.empty() ? 0 : cursor);
        std::ranges::transform(section.data, std::next(image.begin(), static_cast<std::ptrdiff_t>(cursor)), [](char c) {
            return static_cast<std::byte>(c);
        });
        cursor += section.data.size();
    }

    PutLe(image, stringsAt, 4, 4);
    return image;
}

/// Where `BuildCoff` puts the first byte of the first section's DATA.
///
/// Derived from the same constants the builder uses rather than spelled out, so a
/// corruption case cannot quietly start flipping a byte in the header -- part of
/// which the comparison is allowed to overlook, which would make such a case pass
/// for the opposite of its stated reason.
/// @param sectionCount How many sections the image has.
/// @param layout Which header layout it was built to.
/// @return The offset of the first section's data.
[[nodiscard]] constexpr std::size_t StubFirstSectionDataAt(std::size_t sectionCount,
                                                           StubLayout layout = StubLayout::Standard) noexcept
{
    return StubHeaderSize(layout) + (StubSectionHeaderSize * sectionCount);
}

/// The sections a `cl` object carries, near enough for these cases.
/// @param code What `.text$mn` holds.
/// @param objectPath What the driver's path record holds.
/// @param sourceHash What the source checksum record holds.
/// @return The sections, in the order `cl` emits them.
[[nodiscard]] inline std::vector<StubSection> ClSections(std::string code,
                                                         std::string objectPath = "C:\\build\\tu.obj",
                                                         std::string sourceHash = "01234567")
{
    return { { .name = ".drectve", .data = "-defaultlib:libcpmt" },
             { .name = ".debug$S", .data = std::move(objectPath) },
             { .name = ".text$mn", .data = std::move(code) },
             { .name = ".chks64", .data = std::move(sourceHash) } };
}

} // namespace FastCache::Cc::Test
