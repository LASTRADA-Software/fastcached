// SPDX-License-Identifier: Apache-2.0
#include "ObjectEquivalence.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// Read a little-endian unsigned value of @p Width bytes at @p at.
    ///
    /// COFF is little-endian on every machine it is defined for, so this does not
    /// consult the host's byte order -- and must not, or an object copied between a
    /// big-endian host and a little-endian one would compare by two different
    /// layouts. `Core/Endian.hpp` is not reachable here: the launcher does not link
    /// `FastCache`.
    /// @param image The bytes to read from.
    /// @param at Byte offset of the field.
    /// @return The value, or nothing when the field does not fit inside @p image.
    template <std::size_t Width>
    [[nodiscard]] std::optional<std::uint64_t> ReadLe(std::span<std::byte const> image, std::size_t at) noexcept
    {
        static_assert(Width <= 8, "a COFF header field is at most eight bytes wide");
        if (at + Width > image.size())
            return std::nullopt;
        std::uint64_t value = 0;
        for (auto const index: std::views::iota(std::size_t { 0 }, Width))
            value |= static_cast<std::uint64_t>(image[at + index]) << (8U * index);
        return value;
    }

    /// The `TimeDateStamp` field is four bytes wide in both COFF layouts.
    constexpr std::size_t TimeDateStampWidth = 4;

    /// One object-file header layout, and the single field in it that a second
    /// compile of the same source legitimately writes differently.
    ///
    /// A table because there are two of these and MSVC picks between them without
    /// being asked: `/bigobj` is an allowed argument (`CompileJob.cpp`), so such a
    /// build is cacheable and reaches the verifier, and a layout row is what keeps
    /// it from getting a permanent "cannot verify" nobody would investigate. A
    /// third layout is a row.
    struct ObjectLayout
    {
        std::string_view name;                ///< What to call it in a message.
        std::size_t minimumSize {};           ///< Bytes the fixed header occupies.
        std::size_t timeDateStampAt {};       ///< Offset of the clock.
        std::size_t sectionCountAt {};        ///< Offset of `NumberOfSections`.
        std::size_t sectionCountWidth {};     ///< Its width -- two bytes, four on `/bigobj`.
        std::size_t symbolPointerAt {};       ///< Offset of `PointerToSymbolTable`.
        std::size_t symbolCountAt {};         ///< Offset of `NumberOfSymbols`.
        std::size_t symbolRecordSize {};      ///< 18 bytes, 20 on `/bigobj`.
        std::size_t optionalHeaderSizeAt {};  ///< Offset of `SizeOfOptionalHeader`, or `NoOptionalHeader`.
        std::size_t sectionTableAt {};        ///< Where the section table starts, when it is fixed.
    };

    /// `/bigobj` has no optional header, so its section table starts at a fixed
    /// offset rather than one read out of the header.
    constexpr std::size_t NoOptionalHeader = static_cast<std::size_t>(-1);

    /// The ordinary `IMAGE_FILE_HEADER`.
    constexpr ObjectLayout StandardCoff {
        .name = "COFF",
        .minimumSize = 20,
        .timeDateStampAt = 4,
        .sectionCountAt = 2,
        .sectionCountWidth = 2,
        .symbolPointerAt = 8,
        .symbolCountAt = 12,
        .symbolRecordSize = 18,
        .optionalHeaderSizeAt = 16,
        .sectionTableAt = 20,
    };

    /// `ANON_OBJECT_HEADER_BIGOBJ`, which `cl /bigobj` writes and which is a
    /// different structure entirely rather than an extension of the one above.
    constexpr ObjectLayout BigObjCoff {
        .name = "COFF (/bigobj)",
        .minimumSize = 56,
        .timeDateStampAt = 8,
        .sectionCountAt = 44,
        .sectionCountWidth = 4,
        .symbolPointerAt = 48,
        .symbolCountAt = 52,
        .symbolRecordSize = 20,
        .optionalHeaderSizeAt = NoOptionalHeader,
        .sectionTableAt = 56,
    };

    /// A section header is forty bytes in both layouts.
    constexpr std::size_t SectionHeaderSize = 40;

    /// What a `/bigobj` header's `Sig2` holds, where an ordinary object has its
    /// `Machine` field.
    constexpr std::uint64_t BigObjSignature = 0xFFFF;

    /// The lowest `Version` this lays out as `/bigobj`. Below it the same signature
    /// introduces an "anonymous object", which is a different structure again --
    /// refused by name rather than walked as though it were this one.
    constexpr std::uint64_t BigObjMinimumVersion = 2;

    /// ELF's magic, so an ELF object is never mistaken for a COFF one.
    ///
    /// The collision is not hypothetical enough to leave to a heuristic: ELF's
    /// bytes 8..11 are zero padding, which reads as `PointerToSymbolTable == 0` and
    /// satisfies the structural check below, and a large enough object would satisfy
    /// the rest. It would be nearly harmless -- ELF bytes 4..7 are the class, data
    /// encoding, version and ABI, all constant for a given platform -- and "nearly
    /// harmless" is not a property to leave undeclared in the one function that
    /// decides what a verifier is allowed to overlook.
    constexpr std::array<std::byte, 4> ElfMagic { std::byte { 0x7F },
                                                  std::byte { 'E' },
                                                  std::byte { 'L' },
                                                  std::byte { 'F' } };

    /// @param image The bytes to test.
    /// @return True when @p image opens with ELF's magic.
    [[nodiscard]] bool IsElf(std::span<std::byte const> image) noexcept
    {
        return image.size() >= ElfMagic.size() && std::ranges::equal(image.first(ElfMagic.size()), ElfMagic);
    }

    /// Whether @p image carries a `/bigobj` signature, whatever its version.
    ///
    /// Separate from laying it out, because the two answers differ: a signature this
    /// recognises and a version it does not is `Unsupported` -- an MSVC object this
    /// code cannot read -- while no signature at all just means "some other format",
    /// which is compared strictly and correctly.
    /// @param image The bytes to test.
    /// @return True when `Sig1` is zero and `Sig2` is 0xFFFF.
    [[nodiscard]] bool HasBigObjSignature(std::span<std::byte const> image) noexcept
    {
        auto const sig1 = ReadLe<2>(image, 0);
        auto const sig2 = ReadLe<2>(image, 2);
        return sig1.has_value() && sig2.has_value() && *sig1 == 0 && *sig2 == BigObjSignature;
    }

    /// Where the section table starts, per @p layout, for @p image.
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return The offset, or nothing when the optional header size cannot be read.
    [[nodiscard]] std::optional<std::size_t> SectionTableAt(ObjectLayout const& layout,
                                                            std::span<std::byte const> image) noexcept
    {
        if (layout.optionalHeaderSizeAt == NoOptionalHeader)
            return layout.sectionTableAt;
        auto const optional = ReadLe<2>(image, layout.optionalHeaderSizeAt);
        if (!optional.has_value())
            return std::nullopt;
        return layout.sectionTableAt + static_cast<std::size_t>(*optional);
    }

    /// Whether @p image is internally consistent when read by @p layout.
    ///
    /// The test is that everything the header CLAIMS is present actually fits inside
    /// the file. That is what makes it usable as a recogniser as well as a
    /// precondition: an image of another format almost never satisfies it, and one
    /// of this format always does unless it has been truncated.
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return True when the section table and symbol table lie inside @p image.
    [[nodiscard]] bool IsConsistent(ObjectLayout const& layout, std::span<std::byte const> image) noexcept
    {
        if (image.size() < layout.minimumSize)
            return false;
        auto const sections = layout.sectionCountWidth == 2 ? ReadLe<2>(image, layout.sectionCountAt)
                                                            : ReadLe<4>(image, layout.sectionCountAt);
        auto const symbolPointer = ReadLe<4>(image, layout.symbolPointerAt);
        auto const symbolCount = ReadLe<4>(image, layout.symbolCountAt);
        auto const tableAt = SectionTableAt(layout, image);
        if (!sections.has_value() || !symbolPointer.has_value() || !symbolCount.has_value() || !tableAt.has_value())
            return false;
        if (*tableAt + (SectionHeaderSize * *sections) > image.size())
            return false;
        // A symbol table pointer of zero means there is none, which is legal and is
        // not a claim about the file's size.
        if (*symbolPointer == 0)
            return *symbolCount == 0;
        return *symbolPointer + (layout.symbolRecordSize * *symbolCount) <= image.size();
    }

    /// How @p image should be read, if this code knows how.
    struct LayoutChoice
    {
        /// The layout, or nothing when there is none to apply.
        ObjectLayout const* layout { nullptr };
        /// True when @p image is recognisably an MSVC object this code cannot lay
        /// out -- the only thing that earns `Unsupported`. False for a format that
        /// is simply not COFF, which is compared strictly and correctly.
        bool unreadableMsvcObject { false };
    };

    /// Choose a layout for @p image.
    /// @param image The bytes.
    /// @return Which layout applies, or why none does.
    [[nodiscard]] LayoutChoice ChooseLayout(std::span<std::byte const> image) noexcept
    {
        if (IsElf(image))
            return {};
        if (HasBigObjSignature(image))
        {
            auto const version = ReadLe<2>(image, 4);
            if (!version.has_value() || *version < BigObjMinimumVersion || !IsConsistent(BigObjCoff, image))
                return { .layout = nullptr, .unreadableMsvcObject = true };
            return { .layout = &BigObjCoff, .unreadableMsvcObject = false };
        }
        if (IsConsistent(StandardCoff, image))
            return { .layout = &StandardCoff, .unreadableMsvcObject = false };
        return {};
    }

    /// One section, as the descriptive walk sees it.
    struct SectionSpan
    {
        std::string name;
        std::size_t at {};
        std::size_t size {};
    };

    /// The sections of @p image, in file order, with their raw byte ranges.
    ///
    /// Purely descriptive: nothing here can change a verdict, so a section table
    /// this cannot follow costs a vague message rather than a wrong answer.
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return The sections whose data lies inside @p image; empty when the table
    ///         cannot be followed.
    [[nodiscard]] std::vector<SectionSpan> SectionsOf(ObjectLayout const& layout, std::span<std::byte const> image)
    {
        auto const count = layout.sectionCountWidth == 2 ? ReadLe<2>(image, layout.sectionCountAt)
                                                         : ReadLe<4>(image, layout.sectionCountAt);
        auto const tableAt = SectionTableAt(layout, image);
        auto const symbolPointer = ReadLe<4>(image, layout.symbolPointerAt);
        auto const symbolCount = ReadLe<4>(image, layout.symbolCountAt);
        if (!count.has_value() || !tableAt.has_value() || !symbolPointer.has_value() || !symbolCount.has_value())
            return {};

        // A name too long for the eight-byte field is an offset into the string
        // table, which follows the symbol table. COMDAT names reach that length
        // routinely, so this is the ordinary case rather than an exotic one.
        auto const stringsAt = static_cast<std::size_t>(*symbolPointer)
                               + (layout.symbolRecordSize * static_cast<std::size_t>(*symbolCount));

        std::vector<SectionSpan> sections;
        sections.reserve(static_cast<std::size_t>(*count));
        for (auto const index: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(*count)))
        {
            auto const at = *tableAt + (SectionHeaderSize * index);
            auto const size = ReadLe<4>(image, at + 16);
            auto const dataAt = ReadLe<4>(image, at + 20);
            if (!size.has_value() || !dataAt.has_value())
                return sections;

            std::string name;
            for (auto const byte: image.subspan(at, 8))
            {
                if (byte == std::byte { 0 })
                    break;
                name.push_back(static_cast<char>(byte));
            }
            if (name.starts_with('/') && *symbolPointer != 0)
            {
                std::size_t offset = 0;
                auto const digits = std::string_view { name }.substr(1);
                auto const numeric = !digits.empty() && std::ranges::all_of(digits, [](char c) {
                    return c >= '0' && c <= '9';
                });
                if (numeric)
                {
                    for (auto const c: digits)
                        offset = (offset * 10) + static_cast<std::size_t>(c - '0');
                    std::string resolved;
                    for (auto at2 = stringsAt + offset; at2 < image.size() && image[at2] != std::byte { 0 }; ++at2)
                        resolved.push_back(static_cast<char>(image[at2]));
                    if (!resolved.empty())
                        name = resolved;
                }
            }

            auto const start = static_cast<std::size_t>(*dataAt);
            auto const length = static_cast<std::size_t>(*size);
            if (start != 0 && length != 0 && start + length <= image.size())
                sections.push_back({ .name = std::move(name), .at = start, .size = length });
        }
        return sections;
    }

    /// The offsets at which @p left and @p right differ, up to @p limit of them.
    /// @param left One image.
    /// @param right The other, of the same length.
    /// @param limit Stop after this many.
    /// @return The differing offsets, in order.
    [[nodiscard]] std::vector<std::size_t> DifferingOffsets(std::span<std::byte const> left,
                                                            std::span<std::byte const> right,
                                                            std::size_t limit)
    {
        std::vector<std::size_t> offsets;
        for (auto const index: std::views::iota(std::size_t { 0 }, left.size()))
        {
            if (left[index] == right[index])
                continue;
            offsets.push_back(index);
            if (offsets.size() >= limit)
                break;
        }
        return offsets;
    }

    /// How many differing offsets to look at when describing a difference.
    ///
    /// Enough to name every section involved in the cases measured (12 bytes is the
    /// worst of them) with room to spare, and bounded so that two entirely unrelated
    /// objects do not walk a megabyte to say "these differ".
    constexpr std::size_t DescribeBudget = 4096;

    /// Sections `cl` fills with a record of WHERE it compiled rather than WHAT.
    ///
    /// **Not an excuse list.** Nothing here is overlooked; a difference confined to
    /// these still answers `Different`, because a hit whose object was built in
    /// another checkout is exactly
    /// [#489](https://github.com/LASTRADA-Software/fastcached/issues/489) and is the
    /// case an operator runs the verifier to catch. It is a list of names that lets
    /// the MESSAGE say which finding this is -- a foreign build path, or different
    /// code -- because those are acted on differently and "wrong object" alone means
    /// neither.
    constexpr std::array<std::string_view, 2> PathRecordSections { ".debug$S", ".chks64" };

    /// Name the sections @p offsets fall in, in file order and without repeats.
    /// @param sections The section spans of the image.
    /// @param offsets Where the images differ.
    /// @param outsideAny Set when at least one offset falls in no section at all.
    /// @return The section names touched.
    [[nodiscard]] std::vector<std::string> TouchedSections(std::vector<SectionSpan> const& sections,
                                                           std::vector<std::size_t> const& offsets,
                                                           bool& outsideAny)
    {
        std::vector<std::string> touched;
        for (auto const offset: offsets)
        {
            auto const hit = std::ranges::find_if(sections, [offset](SectionSpan const& section) {
                return offset >= section.at && offset < section.at + section.size;
            });
            if (hit == sections.end())
            {
                outsideAny = true;
                continue;
            }
            if (std::ranges::find(touched, hit->name) == touched.end())
                touched.push_back(hit->name);
        }
        return touched;
    }

    /// Render @p names as `a, b and c`.
    /// @param names What to join; never empty.
    /// @return The rendered list.
    [[nodiscard]] std::string JoinNames(std::vector<std::string> const& names)
    {
        std::string out;
        for (auto const index: std::views::iota(std::size_t { 0 }, names.size()))
        {
            if (index != 0)
                out += index + 1 == names.size() ? " and " : ", ";
            out += names[index];
        }
        return out;
    }

    /// Say where two images of the same length differ, as well as can be told.
    ///
    /// @param layout The layout both were read by, or null when neither is COFF.
    /// @param served The cache's bytes.
    /// @param fresh The compiler's bytes.
    /// @param offsets Where they differ; never empty.
    /// @return A sentence naming the difference.
    [[nodiscard]] std::string DescribeDifference(ObjectLayout const* layout,
                                                 std::span<std::byte const> served,
                                                 std::span<std::byte const> fresh,
                                                 std::vector<std::size_t> const& offsets)
    {
        auto const positions = std::format("{} differing byte(s), first at offset {}", offsets.size(), offsets.front());
        if (layout == nullptr)
            return positions;

        auto const sections = SectionsOf(*layout, fresh);
        if (sections.empty())
            return positions;

        auto outsideAny = false;
        auto touched = TouchedSections(sections, offsets, outsideAny);
        if (touched.empty())
            return std::format("{}, outside every section (the header or the symbol table)", positions);

        auto const onlyPathRecords = !outsideAny && std::ranges::all_of(touched, [](std::string const& name) {
            return std::ranges::find(PathRecordSections, name) != PathRecordSections.end();
        });
        if (onlyPathRecords)
            return std::format("{} -- which record WHERE the compile happened, not what it compiled. The cached "
                               "object was built at a different path, in another checkout or on another machine "
                               "(its code and data are identical)",
                               JoinNames(touched));

        // The served image's own section names, only when they disagree: a section
        // that appeared, vanished or moved is a different shape of wrongness from a
        // section whose contents changed, and the two are fixed in different places.
        auto const servedSections = SectionsOf(*layout, served);
        if (servedSections.size() != sections.size())
            return std::format("the cached object has {} section(s) where this compile produces {}",
                               servedSections.size(),
                               sections.size());

        return std::format("{}, in {}", positions, JoinNames(touched));
    }
} // namespace

ObjectComparisonResult CompareObjectImages(std::span<std::byte const> served, std::span<std::byte const> fresh)
{
    if (served.size() == fresh.size()
        && (served.empty() || std::memcmp(served.data(), fresh.data(), served.size()) == 0))
        return { .outcome = ObjectComparison::Identical, .detail = {} };

    // The FRESH image decides, and only the fresh one: it is this toolchain's own
    // output, so a format this cannot lay out is this code's blind spot. Deciding
    // from the served image instead would let a damaged or foreign object talk the
    // verifier out of answering, which is the same silence by another route.
    auto const choice = ChooseLayout(fresh);
    if (choice.unreadableMsvcObject)
        return { .outcome = ObjectComparison::Unsupported,
                 .detail = "this compiler writes an MSVC object format this build cannot lay out, so the clock it "
                           "stamps into every object cannot be told apart from a real difference" };

    if (served.size() != fresh.size())
        return { .outcome = ObjectComparison::Different,
                 .detail = std::format("the cached object is {} bytes where this compile produces {}",
                                       served.size(),
                                       fresh.size()) };

    auto const offsets = DifferingOffsets(served, fresh, DescribeBudget);
    if (choice.layout != nullptr)
    {
        // The one normalised region, and the only one: measured, two compiles of one
        // translation unit to one object path differ here and nowhere else, on both
        // MSVC drivers, with and without `/Z7`. A difference reaching past it is a
        // finding -- see PathRecordSections for why that includes the path records.
        auto const stampAt = choice.layout->timeDateStampAt;
        auto const clockOnly = std::ranges::all_of(offsets, [stampAt](std::size_t offset) {
            return offset >= stampAt && offset < stampAt + TimeDateStampWidth;
        });
        if (clockOnly)
            return { .outcome = ObjectComparison::EquivalentApartFromVolatile,
                     .detail = "the compiler's timestamp, which every MSVC-family driver stamps into the object "
                               "header" };
    }

    return { .outcome = ObjectComparison::Different,
             .detail = DescribeDifference(choice.layout, served, fresh, offsets) };
}

} // namespace FastCache::Cc
