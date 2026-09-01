// SPDX-License-Identifier: Apache-2.0
#include "ObjectEquivalence.hpp"

#include <FastCache/Core/Endian.hpp>

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
    /// Read a little-endian header field at @p at, if it fits.
    ///
    /// The decode is `Core/Endian.hpp`'s. Only the offset and the bounds check are
    /// this file's, and the bounds check is the point: every field here is read out
    /// of an image whose header may be describing a file that is not there.
    /// @param image The bytes to read from.
    /// @param at Byte offset of the field.
    /// @return The value, or nothing when the field does not fit inside @p image.
    template <WireInteger T>
    [[nodiscard]] std::optional<T> ReadLe(std::span<std::byte const> image, std::size_t at) noexcept
    {
        if (at + sizeof(T) > image.size())
            return std::nullopt;
        return ReadLittleEndian<T>(image.subspan(at));
    }

    /// The `TimeDateStamp` field is four bytes wide in both COFF layouts.
    constexpr std::size_t TimeDateStampWidth = 4;

    /// A section header is forty bytes in both layouts.
    constexpr std::size_t SectionHeaderSize = 40;

    /// One object-file header layout: where the fields this comparison needs live.
    ///
    /// A table because there are two of these and MSVC picks between them without
    /// being asked: `/bigobj` is an allowed argument (`CompileJob.cpp`), so such a
    /// build is cacheable and reaches the verifier, and a layout row is what keeps it
    /// from getting a permanent "cannot verify" nobody would investigate. A third
    /// layout is a row.
    ///
    /// Not keyed on the compiler `Flavor`: `/bigobj` is chosen per INVOCATION rather
    /// than per driver, and the served image may have been produced by another driver
    /// on another machine -- so the launcher's own flavour is not an answer about the
    /// bytes in hand. Only the image can say, which is what `ChooseLayout` asks it.
    struct ObjectLayout
    {
        /// Bytes the fixed header occupies -- and therefore where the section table
        /// starts. One field rather than two: the section table begins where the
        /// header ends, so a second spelling could only ever disagree.
        std::size_t headerSize {};
        std::size_t timeDateStampAt {};   ///< Offset of the clock.
        std::size_t sectionCountAt {};    ///< Offset of `NumberOfSections`.
        std::size_t sectionCountWidth {}; ///< Its width -- two bytes, four on `/bigobj`.
        std::size_t symbolPointerAt {};   ///< Offset of `PointerToSymbolTable`.
        std::size_t symbolCountAt {};     ///< Offset of `NumberOfSymbols`.
        std::size_t symbolRecordSize {};  ///< 18 bytes, 20 on `/bigobj`.
        /// Offset of `SizeOfOptionalHeader`, or nothing where the layout has no
        /// optional header at all -- which is a different fact from one of size zero.
        std::optional<std::size_t> optionalHeaderSizeAt {};
    };

    /// The ordinary `IMAGE_FILE_HEADER`.
    constexpr ObjectLayout StandardCoff {
        .headerSize = 20,
        .timeDateStampAt = 4,
        .sectionCountAt = 2,
        .sectionCountWidth = 2,
        .symbolPointerAt = 8,
        .symbolCountAt = 12,
        .symbolRecordSize = 18,
        .optionalHeaderSizeAt = 16,
    };

    /// `ANON_OBJECT_HEADER_BIGOBJ`, which `cl /bigobj` writes and which is a
    /// different structure entirely rather than an extension of the one above.
    constexpr ObjectLayout BigObjCoff {
        .headerSize = 56,
        .timeDateStampAt = 8,
        .sectionCountAt = 44,
        .sectionCountWidth = 4,
        .symbolPointerAt = 48,
        .symbolCountAt = 52,
        .symbolRecordSize = 20,
        .optionalHeaderSizeAt = std::nullopt,
    };

    /// What a `/bigobj` header's `Sig2` holds, where an ordinary object has its
    /// `Machine` field.
    constexpr std::uint16_t BigObjSignature = 0xFFFF;

    /// The lowest `Version` this lays out as `/bigobj`. Below it the same signature
    /// introduces an "anonymous object", which is a different structure again --
    /// refused by name rather than walked as though it were this one.
    constexpr std::uint16_t BigObjMinimumVersion = 2;

    /// ELF's magic, so an ELF object is never mistaken for a COFF one.
    ///
    /// The collision is not hypothetical enough to leave to a heuristic: ELF's bytes
    /// 8..11 are zero padding, which reads as `PointerToSymbolTable == 0` and
    /// satisfies the structural check below, and a large enough object would satisfy
    /// the rest. It would be nearly harmless -- ELF bytes 4..7 are the class, data
    /// encoding, version and ABI, all constant for a given platform -- and "nearly
    /// harmless" is not a property to leave undeclared in the one function that
    /// decides what a verifier is allowed to overlook.
    constexpr std::array<std::byte, 4> ElfMagic {
        std::byte { 0x7F }, std::byte { 'E' }, std::byte { 'L' }, std::byte { 'F' }
    };

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
        auto const sig1 = ReadLe<std::uint16_t>(image, 0);
        auto const sig2 = ReadLe<std::uint16_t>(image, 2);
        return sig1.has_value() && sig2.has_value() && *sig1 == 0 && *sig2 == BigObjSignature;
    }

    /// `NumberOfSections`, read at this layout's width.
    ///
    /// One spelling, because the width is the whole difference between the two
    /// layouts here and a second copy of the ternary is a second place to get it
    /// wrong. Returned as the wide type: a `/bigobj` count is a full 32 bits, and a
    /// caller that narrowed it would be discarding exactly the value that makes the
    /// bounds check below worth doing.
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return The count, or nothing when the field does not fit.
    [[nodiscard]] std::optional<std::uint64_t> SectionCount(ObjectLayout const& layout,
                                                            std::span<std::byte const> image) noexcept
    {
        if (layout.sectionCountWidth == sizeof(std::uint16_t))
            return ReadLe<std::uint16_t>(image, layout.sectionCountAt);
        return ReadLe<std::uint32_t>(image, layout.sectionCountAt);
    }

    /// Where the section table starts, per @p layout, for @p image.
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return The offset, or nothing when the optional header size cannot be read.
    [[nodiscard]] std::optional<std::size_t> SectionTableAt(ObjectLayout const& layout,
                                                            std::span<std::byte const> image) noexcept
    {
        if (!layout.optionalHeaderSizeAt.has_value())
            return layout.headerSize;
        auto const optional = ReadLe<std::uint16_t>(image, *layout.optionalHeaderSizeAt);
        if (!optional.has_value())
            return std::nullopt;
        return layout.headerSize + static_cast<std::size_t>(*optional);
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
        if (image.size() < layout.headerSize)
            return false;
        auto const sections = SectionCount(layout, image);
        auto const symbolPointer = ReadLe<std::uint32_t>(image, layout.symbolPointerAt);
        auto const symbolCount = ReadLe<std::uint32_t>(image, layout.symbolCountAt);
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
    /// @param image The bytes.
    /// @return The layout, or null when there is none to apply -- which covers both
    ///         "some other format" and an MSVC object this cannot lay out. The two
    ///         are told apart at the one call site that cares, by asking
    ///         `HasBigObjSignature`, because only a `/bigobj` signature can be
    ///         recognised and still not laid out.
    [[nodiscard]] ObjectLayout const* ChooseLayout(std::span<std::byte const> image) noexcept
    {
        if (IsElf(image))
            return nullptr;
        if (HasBigObjSignature(image))
        {
            auto const version = ReadLe<std::uint16_t>(image, 4);
            if (!version.has_value() || *version < BigObjMinimumVersion || !IsConsistent(BigObjCoff, image))
                return nullptr;
            return &BigObjCoff;
        }
        return IsConsistent(StandardCoff, image) ? &StandardCoff : nullptr;
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
    /// Purely descriptive: nothing here can change a verdict, so a section table this
    /// cannot follow costs a vague message rather than a wrong answer.
    ///
    /// A name too long for the eight-byte field is left as the `/nnn` string-table
    /// offset it is spelled as, rather than resolved. Resolution cannot reach any
    /// answer: the only names read back are `.debug$S` and `.chks64`, which are eight
    /// and seven bytes, so neither can ever arrive in the indirect form.
    ///
    /// @param layout The layout to read by.
    /// @param image The bytes.
    /// @return The sections whose data lies inside @p image; empty when the table
    ///         cannot be followed.
    [[nodiscard]] std::vector<SectionSpan> SectionsOf(ObjectLayout const& layout, std::span<std::byte const> image)
    {
        // Asked of THIS image rather than inherited from the caller's, and that is not
        // belt-and-braces: the only image known to be consistent is the fresh one, and
        // this also walks the SERVED image, whose header is whatever a cache handed
        // over. A `/bigobj` `NumberOfSections` is a full 32 bits, so an unchecked one
        // reserves for four billion sections and takes the process down -- from a
        // function whose entire job is to make an error message more specific.
        if (!IsConsistent(layout, image))
            return {};

        auto const count = SectionCount(layout, image);
        auto const tableAt = SectionTableAt(layout, image);
        if (!count.has_value() || !tableAt.has_value())
            return {};

        std::vector<SectionSpan> sections;
        sections.reserve(static_cast<std::size_t>(*count));
        for (auto const index: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(*count)))
        {
            auto const at = *tableAt + (SectionHeaderSize * index);
            auto const size = ReadLe<std::uint32_t>(image, at + 16);
            auto const dataAt = ReadLe<std::uint32_t>(image, at + 20);
            if (!size.has_value() || !dataAt.has_value())
                return sections;

            auto const field = image.subspan(at, 8);
            auto const stop = std::ranges::find(field, std::byte { 0 });
            std::string name;
            std::ranges::transform(
                field.begin(), stop, std::back_inserter(name), [](std::byte byte) { return static_cast<char>(byte); });

            auto const start = static_cast<std::size_t>(*dataAt);
            auto const length = static_cast<std::size_t>(*size);
            if (start != 0 && length != 0 && start + length <= image.size())
                sections.push_back({ .name = std::move(name), .at = start, .size = length });
        }
        return sections;
    }

    /// How many differing offsets to look at when describing a difference.
    ///
    /// Enough to name every section involved in the cases measured (twelve bytes is
    /// the worst of them) with room to spare, and bounded so that two entirely
    /// unrelated objects do not walk a megabyte to say "these differ".
    constexpr std::size_t DescribeBudget = 4096;

    /// The offsets at which @p left and @p right differ, up to `DescribeBudget`.
    ///
    /// Driven by `std::mismatch` rather than a byte loop, because the equal RUNS are
    /// what dominate: the ordinary Windows hit differs in four bytes of a file that
    /// may be megabytes, and a per-byte compare would walk all of it scalar. This
    /// skips each equal run at `memcmp` speed and stops at the budget.
    ///
    /// @param left One image.
    /// @param right The other, of the same length.
    /// @return The differing offsets, in order.
    [[nodiscard]] std::vector<std::size_t> DifferingOffsets(std::span<std::byte const> left,
                                                            std::span<std::byte const> right)
    {
        std::vector<std::size_t> offsets;
        std::size_t at = 0;
        while (at < left.size() && offsets.size() < DescribeBudget)
        {
            auto const [l, r] = std::ranges::mismatch(left.subspan(at), right.subspan(at));
            if (l == left.subspan(at).end())
                break;
            at += static_cast<std::size_t>(std::ranges::distance(left.subspan(at).begin(), l));
            offsets.push_back(at);
            ++at;
        }
        return offsets;
    }

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

    /// Which sections a set of differing offsets falls in.
    struct TouchedSections
    {
        /// The section names touched, in file order and without repeats.
        std::vector<std::string> names;
        /// Whether at least one offset fell in no section at all -- the header, the
        /// symbol table, or padding between sections.
        bool anyOutside { false };
    };

    /// Name the sections @p offsets fall in.
    /// @param sections The section spans of the image.
    /// @param offsets Where the images differ.
    /// @return The names touched, and whether anything fell outside them all.
    [[nodiscard]] TouchedSections SectionsTouchedBy(std::vector<SectionSpan> const& sections,
                                                    std::vector<std::size_t> const& offsets)
    {
        TouchedSections touched;
        for (auto const offset: offsets)
        {
            auto const hit = std::ranges::find_if(sections, [offset](SectionSpan const& section) {
                return offset >= section.at && offset < section.at + section.size;
            });
            if (hit == sections.end())
            {
                touched.anyOutside = true;
                continue;
            }
            if (std::ranges::find(touched.names, hit->name) == touched.names.end())
                touched.names.push_back(hit->name);
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
        // Not `const`: it is returned by value on three paths, and constness would
        // turn each of those into a copy.
        auto positions = std::format("{} differing byte(s), first at offset {}", offsets.size(), offsets.front());
        if (layout == nullptr)
            return positions;

        // The served image's own section COUNT, only when it disagrees: a section that
        // appeared or vanished is a different shape of wrongness from a section whose
        // contents changed, and the two are fixed in different places. A count rather
        // than a walk, because a count is all this asks -- and on a `/Gy` object a walk
        // is one heap-allocated name per function.
        auto const servedCount = IsConsistent(*layout, served) ? SectionCount(*layout, served) : std::nullopt;
        auto const freshCount = SectionCount(*layout, fresh);
        if (servedCount != freshCount)
            return std::format("the cached object has {} section(s) where this compile produces {}",
                               servedCount.has_value() ? std::format("{}", *servedCount) : "an unreadable number of",
                               freshCount.has_value() ? std::format("{}", *freshCount) : "an unreadable number");

        auto const sections = SectionsOf(*layout, fresh);
        if (sections.empty())
            return positions;

        auto const touched = SectionsTouchedBy(sections, offsets);
        if (touched.names.empty())
            return std::format("{}, outside every section (the header or the symbol table)", positions);

        auto const onlyPathRecords = !touched.anyOutside && std::ranges::all_of(touched.names, [](auto const& name) {
            return std::ranges::find(PathRecordSections, name) != PathRecordSections.end();
        });
        if (onlyPathRecords)
            return std::format("{} -- which record WHERE the compile happened, not what it compiled. The cached "
                               "object was built at a different path, in another checkout or on another machine "
                               "(its code and data are identical)",
                               JoinNames(touched.names));

        return std::format("{}, in {}", positions, JoinNames(touched.names));
    }
} // namespace

ObjectComparisonResult CompareObjectImages(std::span<std::byte const> served, std::span<std::byte const> fresh)
{
    if (served.size() == fresh.size() && (served.empty() || std::memcmp(served.data(), fresh.data(), served.size()) == 0))
        return { .outcome = ObjectComparison::Identical };

    // The FRESH image decides which layout applies, and only the fresh one: it is
    // this toolchain's own output, so a format this cannot lay out is this code's
    // blind spot. Deciding from the served image instead would let a damaged or
    // foreign object talk the verifier out of answering, which is the same silence
    // by another route.
    auto const* layout = ChooseLayout(fresh);
    if (layout == nullptr && HasBigObjSignature(fresh))
        return { .outcome = ObjectComparison::Unsupported,
                 .detail = "this compiler writes an MSVC object format this build cannot lay out, so the clock it "
                           "stamps into every object cannot be told apart from a real difference" };

    if (served.size() != fresh.size())
        return { .outcome = ObjectComparison::Different,
                 .detail = std::format(
                     "the cached object is {} bytes where this compile produces {}", served.size(), fresh.size()) };

    auto offsets = DifferingOffsets(served, fresh);

    // Drop the one normalised region, and only it. A difference reaching past it is a
    // finding -- see PathRecordSections for why that includes the path records.
    //
    // Dropped BEFORE the description as well as before the decision, because the
    // clock lies in the header rather than in any section: left in, it made every
    // difference look as though it reached outside the sections, which suppressed the
    // classification that tells a foreign build path from stale code.
    if (layout != nullptr)
    {
        auto const stampAt = layout->timeDateStampAt;
        auto const removed = std::ranges::remove_if(
            offsets, [stampAt](std::size_t offset) { return offset >= stampAt && offset < stampAt + TimeDateStampWidth; });
        offsets.erase(removed.begin(), removed.end());
    }

    if (offsets.empty())
        return { .outcome = ObjectComparison::EquivalentApartFromVolatile,
                 .detail = "the compiler's timestamp, which every MSVC-family driver stamps into the object header" };

    return { .outcome = ObjectComparison::Different, .detail = DescribeDifference(layout, served, fresh, offsets) };
}

} // namespace FastCache::Cc
