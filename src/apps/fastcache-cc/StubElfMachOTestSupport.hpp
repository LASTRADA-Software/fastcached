// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace FastCache::Cc::Test
{

/// Building ELF and Mach-O object images for tests, in ONE place -- the COFF builder's
/// siblings, for the reason `StubCoffTestSupport.hpp` gives for being shared.
///
/// Synthetic rather than compiled because the formats a host can produce are the host's:
/// a Windows run has no ELF compiler and a Linux run no Mach-O one, and the walk has to
/// be right about all three everywhere the launcher runs.

/// One section of a stub ELF or Mach-O object.
struct StubFormatSection
{
    /// ELF: the section name. Mach-O: the SECTION name; the segment is `segment`.
    std::string name;
    std::string data;
    /// Mach-O only: the segment the section belongs to.
    std::string segment { "__TEXT" };
    /// ELF `sh_type` (`SHT_PROGBITS` by default; 8 is `SHT_NOBITS`).
    std::uint32_t type { 1 };
    /// ELF `sh_flags` (`SHF_ALLOC` by default; 0x800 is `SHF_COMPRESSED`).
    std::uint64_t flags { 0x2 };
};

/// Write @p value into @p bytes at @p at over @p width bytes, in either byte order.
/// @param bytes The image being built.
/// @param at Where the field starts.
/// @param width How wide it is.
/// @param value What to store.
/// @param bigEndian Whether the image is big-endian.
inline void PutField(std::vector<std::byte>& bytes, std::size_t at, std::size_t width, std::uint64_t value, bool bigEndian)
{
    if (at + width > bytes.size())
        return;
    for (auto const index: std::views::iota(std::size_t { 0 }, width))
    {
        auto const shift = bigEndian ? 8U * (width - 1 - index) : 8U * index;
        bytes[at + index] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

/// Copy @p text into @p bytes at @p at.
inline void PutText(std::vector<std::byte>& bytes, std::size_t at, std::string const& text)
{
    if (at + text.size() > bytes.size())
        return;
    std::ranges::transform(
        text, std::next(bytes.begin(), static_cast<std::ptrdiff_t>(at)), [](char c) { return static_cast<std::byte>(c); });
}

/// How a stub ELF image is shaped.
struct StubElfShape
{
    bool is64 { true };
    bool bigEndian { false };
    /// Store the section count and the name table's index the way an object with more
    /// than 65279 sections must: in section 0.
    bool extendedNumbering { false };
};

/// Build a structurally valid relocatable ELF image: the header, each section's data,
/// the section-name table, then the section header table (null section first).
/// @param sections The sections, in order.
/// @param shape Class, byte order and numbering.
/// @return The image.
[[nodiscard]] inline std::vector<std::byte> BuildElf(std::vector<StubFormatSection> const& sections, StubElfShape shape = {})
{
    auto const headerSize = shape.is64 ? std::size_t { 64 } : std::size_t { 52 };
    auto const entrySize = shape.is64 ? std::size_t { 64 } : std::size_t { 40 };
    auto const word = shape.is64 ? std::size_t { 8 } : std::size_t { 4 };

    std::string names(1, '\0');
    std::vector<std::size_t> nameAt;
    for (auto const& section: sections)
    {
        nameAt.push_back(names.size());
        names += section.name;
        names.push_back('\0');
    }
    auto const shstrtabName = names.size();
    names += ".shstrtab";
    names.push_back('\0');

    std::vector<std::size_t> dataAt;
    auto cursor = headerSize;
    for (auto const& section: sections)
    {
        dataAt.push_back(cursor);
        if (section.type != 8)
            cursor += section.data.size();
    }
    auto const shstrtabData = cursor;
    cursor += names.size();
    auto const tableAt = cursor;
    auto const count = sections.size() + 2; // the null section and `.shstrtab`
    std::vector<std::byte> image(tableAt + (count * entrySize), std::byte { 0 });

    PutText(image,
            0,
            std::string { "\x7F"
                          "ELF" });
    image[4] = static_cast<std::byte>(shape.is64 ? 2 : 1);
    image[5] = static_cast<std::byte>(shape.bigEndian ? 2 : 1);
    image[6] = std::byte { 1 };
    auto const big = shape.bigEndian;
    PutField(image, 16, 2, 1, big); // ET_REL
    PutField(image, 20, 4, 1, big);
    PutField(image, shape.is64 ? 0x28 : 0x20, word, tableAt, big);
    PutField(image, shape.is64 ? 0x34 : 0x28, 2, headerSize, big);
    PutField(image, shape.is64 ? 0x3A : 0x2E, 2, entrySize, big);
    PutField(image, shape.is64 ? 0x3C : 0x30, 2, shape.extendedNumbering ? 0 : count, big);
    PutField(image, shape.is64 ? 0x3E : 0x32, 2, shape.extendedNumbering ? 0xFFFF : count - 1, big);

    auto const writeHeader = [&](std::size_t index,
                                 std::size_t name,
                                 std::uint32_t type,
                                 std::uint64_t flags,
                                 std::uint64_t offset,
                                 std::uint64_t size,
                                 std::uint32_t link) {
        auto const at = tableAt + (index * entrySize);
        PutField(image, at, 4, name, big);
        PutField(image, at + 4, 4, type, big);
        PutField(image, at + 8, word, flags, big);
        PutField(image, at + (shape.is64 ? 24 : 16), word, offset, big);
        PutField(image, at + (shape.is64 ? 32 : 20), word, size, big);
        PutField(image, at + (shape.is64 ? 40 : 24), 4, link, big);
    };

    // Section 0: null, unless it carries the extended count and name-table index.
    writeHeader(0,
                0,
                0,
                0,
                0,
                shape.extendedNumbering ? count : 0,
                shape.extendedNumbering ? static_cast<std::uint32_t>(count - 1) : 0);
    for (auto const index: std::views::iota(std::size_t { 0 }, sections.size()))
    {
        auto const& section = sections[index];
        if (section.type != 8)
            PutText(image, dataAt[index], section.data);
        writeHeader(index + 1, nameAt[index], section.type, section.flags, dataAt[index], section.data.size(), 0);
    }
    PutText(image, shstrtabData, names);
    writeHeader(count - 1, shstrtabName, 3, 0, shstrtabData, names.size(), 0);
    return image;
}

/// Build a thin little-endian 64-bit Mach-O object: the header, one `LC_SEGMENT_64`
/// holding every section, then the sections' data.
/// @param sections The sections, in order; each names its own segment.
/// @return The image.
[[nodiscard]] inline std::vector<std::byte> BuildMachO64(std::vector<StubFormatSection> const& sections)
{
    constexpr std::size_t HeaderSize = 32;
    constexpr std::size_t SegmentSize = 72;
    constexpr std::size_t SectionSize = 80;
    auto const commandSize = SegmentSize + (SectionSize * sections.size());
    auto cursor = HeaderSize + commandSize;
    std::vector<std::size_t> dataAt;
    for (auto const& section: sections)
    {
        dataAt.push_back(cursor);
        cursor += section.data.size();
    }
    std::vector<std::byte> image(cursor, std::byte { 0 });

    PutField(image, 0, 4, 0xFEEDFACF, false);
    PutField(image, 4, 4, 0x0100000C, false); // CPU_TYPE_ARM64
    PutField(image, 12, 4, 1, false);         // MH_OBJECT
    PutField(image, 16, 4, 1, false);         // one load command
    PutField(image, 20, 4, commandSize, false);

    PutField(image, HeaderSize, 4, 0x19, false); // LC_SEGMENT_64
    PutField(image, HeaderSize + 4, 4, commandSize, false);
    PutField(image, HeaderSize + 64, 4, sections.size(), false);
    for (auto const index: std::views::iota(std::size_t { 0 }, sections.size()))
    {
        auto const& section = sections[index];
        auto const at = HeaderSize + SegmentSize + (index * SectionSize);
        PutText(image, at, section.name.substr(0, 16));
        PutText(image, at + 16, section.segment.substr(0, 16));
        PutField(image, at + 40, 8, section.data.size(), false);
        PutField(image, at + 48, 4, dataAt[index], false);
        PutText(image, dataAt[index], section.data);
    }
    return image;
}

/// Wrap thin Mach-O images in a universal (fat) file, 32-bit table.
/// @param slices The thin images, in order.
/// @return The image.
[[nodiscard]] inline std::vector<std::byte> BuildFatMachO(std::vector<std::vector<std::byte>> const& slices)
{
    constexpr std::size_t Alignment = 16;
    auto cursor = 8 + (20 * slices.size());
    std::vector<std::pair<std::size_t, std::size_t>> placed;
    for (auto const& slice: slices)
    {
        cursor = (cursor + Alignment - 1) / Alignment * Alignment;
        placed.emplace_back(cursor, slice.size());
        cursor += slice.size();
    }
    std::vector<std::byte> image(cursor, std::byte { 0 });
    PutField(image, 0, 4, 0xCAFEBABE, true);
    PutField(image, 4, 4, slices.size(), true);
    for (auto const index: std::views::iota(std::size_t { 0 }, slices.size()))
    {
        auto const at = 8 + (20 * index);
        PutField(image, at + 8, 4, placed[index].first, true);
        PutField(image, at + 12, 4, placed[index].second, true);
        PutField(image, at + 16, 4, 4, true);
        std::ranges::copy(slices[index], std::next(image.begin(), static_cast<std::ptrdiff_t>(placed[index].first)));
    }
    return image;
}

} // namespace FastCache::Cc::Test
