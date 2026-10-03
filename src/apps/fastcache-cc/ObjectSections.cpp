// SPDX-License-Identifier: Apache-2.0
#include "ObjectSections.hpp"

#include <FastCache/Core/Endian.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <iterator>
#include <ranges>
#include <string_view>
#include <utility>

namespace FastCache::Cc
{

namespace
{
    /// Read a header field at @p at, if it fits, in the byte order the image declares.
    ///
    /// The decode is `Core/Endian.hpp`'s. Only the offset and the bounds check are this
    /// file's, and the bounds check is the point: every field here is read out of an
    /// image whose header may be describing a file that is not there.
    struct FieldReader
    {
        std::span<std::byte const> image;
        bool bigEndian { false };

        /// @param at Byte offset of the field.
        /// @return The value, or nothing when the field does not fit inside the image.
        template <WireInteger T>
        [[nodiscard]] std::optional<T> At(std::size_t at) const noexcept
        {
            if (at > image.size() || sizeof(T) > image.size() - at)
                return std::nullopt;
            return bigEndian ? ReadBigEndian<T>(image.subspan(at)) : ReadLittleEndian<T>(image.subspan(at));
        }
    };

    /// @param image The bytes. @return A little-endian reader over them.
    [[nodiscard]] FieldReader LittleEndian(std::span<std::byte const> image) noexcept
    {
        return FieldReader { .image = image, .bigEndian = false };
    }

    /// Whether `[at, at + size)` lies inside an image of @p imageSize bytes, without
    /// the addition overflowing on a header that claims a huge extent.
    [[nodiscard]] constexpr bool Fits(std::uint64_t at, std::uint64_t size, std::size_t imageSize) noexcept
    {
        return at <= imageSize && size <= imageSize - at;
    }

    /// The characters of a fixed-width, NUL-padded name field.
    [[nodiscard]] std::string FixedName(std::span<std::byte const> field)
    {
        auto const stop = std::ranges::find(field, std::byte { 0 });
        std::string name;
        std::ranges::transform(
            field.begin(), stop, std::back_inserter(name), [](std::byte byte) { return static_cast<char>(byte); });
        return name;
    }

    /// The NUL-terminated string at @p at, or nothing when it does not end inside the
    /// image.
    [[nodiscard]] std::optional<std::string> TerminatedName(std::span<std::byte const> image, std::size_t at)
    {
        if (at >= image.size())
            return std::nullopt;
        auto const tail = image.subspan(at);
        auto const stop = std::ranges::find(tail, std::byte { 0 });
        if (stop == tail.end())
            return std::nullopt;
        return FixedName(tail.first(static_cast<std::size_t>(std::ranges::distance(tail.begin(), stop)) + 1));
    }

    // --- COFF ------------------------------------------------------------------

    /// A section header is forty bytes in both COFF layouts.
    constexpr std::size_t CoffSectionHeaderSize = 40;

    /// The ordinary `IMAGE_FILE_HEADER`.
    constexpr CoffLayout StandardCoff {
        .headerSize = 20,
        .timeDateStampAt = 4,
        .sectionCountAt = 2,
        .sectionCountWidth = 2,
        .symbolPointerAt = 8,
        .symbolCountAt = 12,
        .symbolRecordSize = 18,
        .optionalHeaderSizeAt = 16,
    };

    /// `ANON_OBJECT_HEADER_BIGOBJ`, which `cl /bigobj` writes and which is a different
    /// structure entirely rather than an extension of the one above.
    constexpr CoffLayout BigObjCoff {
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
    /// introduces an "anonymous object" -- what `/GL` writes -- which is a different
    /// structure again, refused by name rather than walked as though it were this one.
    constexpr std::uint16_t BigObjMinimumVersion = 2;

    /// Magics of formats that are definitely NOT COFF.
    ///
    /// An exclusion list rather than a list of accepted `Machine` values, and the
    /// direction is the whole point: an unrecognised COFF *machine* must still be laid
    /// out, or the next architecture MSVC targets reads as an unknown format.
    ///
    /// The collision is not hypothetical enough to leave to `IsConsistentCoff`, which is
    /// a plausibility test rather than a positive identification. ELF's bytes 8..11 are
    /// zero padding, which reads as `PointerToSymbolTable == 0`; a 64-bit Mach-O's
    /// `NumberOfSections` reads as 0xFEED, its `PointerToSymbolTable` as the CPU subtype
    /// and its `NumberOfSymbols` as the file type -- so a large enough object of either
    /// satisfies every bound and would be laid out as a COFF file with an invented
    /// section table.
    constexpr std::array<std::array<std::byte, 4>, 7> NotCoffMagics { {
        // ELF, either endianness of the following fields -- the magic itself is fixed.
        { std::byte { 0x7F }, std::byte { 'E' }, std::byte { 'L' }, std::byte { 'F' } },
        // Mach-O, 32- and 64-bit, and their byte-swapped spellings.
        { std::byte { 0xFE }, std::byte { 0xED }, std::byte { 0xFA }, std::byte { 0xCE } },
        { std::byte { 0xFE }, std::byte { 0xED }, std::byte { 0xFA }, std::byte { 0xCF } },
        { std::byte { 0xCE }, std::byte { 0xFA }, std::byte { 0xED }, std::byte { 0xFE } },
        { std::byte { 0xCF }, std::byte { 0xFA }, std::byte { 0xED }, std::byte { 0xFE } },
        // A universal (fat) Mach-O, 32- and 64-bit tables.
        { std::byte { 0xCA }, std::byte { 0xFE }, std::byte { 0xBA }, std::byte { 0xBE } },
        { std::byte { 0xCA }, std::byte { 0xFE }, std::byte { 0xBA }, std::byte { 0xBF } },
    } };

    /// @param image The bytes to test.
    /// @return True when @p image opens with the magic of a format that is not COFF.
    [[nodiscard]] bool IsKnownNonCoff(std::span<std::byte const> image) noexcept
    {
        return std::ranges::any_of(NotCoffMagics, [image](auto const& magic) {
            return image.size() >= magic.size() && std::ranges::equal(image.first(magic.size()), magic);
        });
    }

    /// Where the section table starts, per @p layout, for @p image.
    [[nodiscard]] std::optional<std::size_t> CoffSectionTableAt(CoffLayout const& layout,
                                                                std::span<std::byte const> image) noexcept
    {
        if (!layout.optionalHeaderSizeAt.has_value())
            return layout.headerSize;
        auto const optional = LittleEndian(image).At<std::uint16_t>(*layout.optionalHeaderSizeAt);
        if (!optional.has_value())
            return std::nullopt;
        return layout.headerSize + static_cast<std::size_t>(*optional);
    }

    /// Resolve a COFF section name, following a `/nnn` long name into the string table.
    ///
    /// A long name matters to one reader: DWARF-in-COFF sections (`.debug_info` from
    /// `clang-cl -gdwarf` or a MinGW driver) are longer than the eight-byte field, and
    /// left as `/4` they could not be told from program data. The `//`-prefixed base-64
    /// spelling, which a string table only needs past ten million bytes, is left as
    /// spelled -- a name nothing recognises, which a reader must treat as program data.
    /// @param field The eight-byte name field.
    /// @param strings The string table, possibly empty.
    /// @return The name.
    [[nodiscard]] std::string CoffSectionName(std::span<std::byte const> field, std::span<std::byte const> strings)
    {
        auto name = FixedName(field);
        if (name.size() < 2 || name.front() != '/' || name[1] == '/')
            return name;
        std::size_t offset = 0;
        auto const* const first = std::next(name.data());
        auto const* const last = std::next(name.data(), static_cast<std::ptrdiff_t>(name.size()));
        auto const [end, error] = std::from_chars(first, last, offset);
        if (error != std::errc {} || end != last)
            return name;
        return TerminatedName(strings, offset).value_or(name);
    }

    /// One COFF walk, reporting whether it saw every section with data.
    struct CoffWalk
    {
        std::vector<ObjectSection> sections;
        bool complete { false };
    };

    [[nodiscard]] CoffWalk WalkCoff(CoffLayout const& layout, std::span<std::byte const> image)
    {
        // Asked of THIS image rather than inherited from the caller's: `ObjectEquivalence`
        // also walks the SERVED image, whose header is whatever a cache handed over. A
        // `/bigobj` `NumberOfSections` is a full 32 bits, so an unchecked one reserves
        // for four billion sections and takes the process down.
        if (!IsConsistentCoff(layout, image))
            return {};

        auto const reader = LittleEndian(image);
        auto const count = CoffSectionCount(layout, image);
        auto const tableAt = CoffSectionTableAt(layout, image);
        auto const symbolsAt = reader.At<std::uint32_t>(layout.symbolPointerAt);
        auto const symbols = reader.At<std::uint32_t>(layout.symbolCountAt);
        if (!count.has_value() || !tableAt.has_value() || !symbolsAt.has_value() || !symbols.has_value())
            return {};

        // The string table follows the symbol table; `IsConsistentCoff` has already said
        // the symbol table fits, so the start is inside the image or exactly at its end.
        auto const stringsAt =
            static_cast<std::size_t>(*symbolsAt) + (layout.symbolRecordSize * static_cast<std::size_t>(*symbols));
        auto const strings =
            *symbolsAt != 0 && stringsAt <= image.size() ? image.subspan(stringsAt) : std::span<std::byte const> {};

        CoffWalk walk { .sections = {}, .complete = true };
        walk.sections.reserve(static_cast<std::size_t>(*count));
        for (auto const index: std::views::iota(std::size_t { 0 }, static_cast<std::size_t>(*count)))
        {
            auto const at = *tableAt + (CoffSectionHeaderSize * index);
            auto const size = reader.At<std::uint32_t>(at + 16);
            auto const dataAt = reader.At<std::uint32_t>(at + 20);
            if (!size.has_value() || !dataAt.has_value())
            {
                walk.complete = false;
                return walk;
            }
            // No data in the file: `.bss`, or an empty section.
            if (*dataAt == 0 || *size == 0)
                continue;
            if (!Fits(*dataAt, *size, image.size()))
            {
                walk.complete = false;
                continue;
            }
            walk.sections.push_back({ .name = CoffSectionName(image.subspan(at, 8), strings),
                                      .at = static_cast<std::size_t>(*dataAt),
                                      .size = static_cast<std::size_t>(*size),
                                      .compressed = false });
        }
        return walk;
    }

    // --- ELF -------------------------------------------------------------------

    /// Where the fields of one ELF class live. A table because ELF32 and ELF64 differ in
    /// every offset past the identification bytes, and nothing else.
    struct ElfClass
    {
        std::size_t sectionTableAt {};     ///< `e_shoff`.
        std::size_t sectionTableWidth {};  ///< Its width.
        std::size_t sectionEntrySizeAt {}; ///< `e_shentsize`.
        std::size_t sectionCountAt {};     ///< `e_shnum`.
        std::size_t namesIndexAt {};       ///< `e_shstrndx`.
        std::size_t entrySize {};          ///< The smallest `sh_*` record this reads.
        std::size_t flagsAt {};            ///< `sh_flags`, within a section header.
        std::size_t wordWidth {};          ///< Width of `sh_flags`, `sh_offset`, `sh_size`.
        std::size_t offsetAt {};           ///< `sh_offset`.
        std::size_t sizeAt {};             ///< `sh_size`.
        std::size_t linkAt {};             ///< `sh_link`.
    };

    constexpr ElfClass Elf32 { .sectionTableAt = 0x20,
                               .sectionTableWidth = 4,
                               .sectionEntrySizeAt = 0x2E,
                               .sectionCountAt = 0x30,
                               .namesIndexAt = 0x32,
                               .entrySize = 40,
                               .flagsAt = 8,
                               .wordWidth = 4,
                               .offsetAt = 16,
                               .sizeAt = 20,
                               .linkAt = 24 };

    constexpr ElfClass Elf64 { .sectionTableAt = 0x28,
                               .sectionTableWidth = 8,
                               .sectionEntrySizeAt = 0x3A,
                               .sectionCountAt = 0x3C,
                               .namesIndexAt = 0x3E,
                               .entrySize = 64,
                               .flagsAt = 8,
                               .wordWidth = 8,
                               .offsetAt = 24,
                               .sizeAt = 32,
                               .linkAt = 40 };

    constexpr std::uint32_t ElfSectionNoBits = 8;         ///< `SHT_NOBITS`.
    constexpr std::uint64_t ElfSectionCompressed = 0x800; ///< `SHF_COMPRESSED`.
    constexpr std::uint16_t ElfExtendedIndex = 0xFFFF;    ///< `SHN_XINDEX`.

    /// A word at @p width: the one field whose width differs between the 32- and 64-bit
    /// spellings of both ELF and Mach-O.
    [[nodiscard]] std::optional<std::uint64_t> WordAt(FieldReader const& reader, std::size_t width, std::size_t at) noexcept
    {
        if (width == sizeof(std::uint32_t))
            return reader.At<std::uint32_t>(at);
        return reader.At<std::uint64_t>(at);
    }

    /// One ELF section header, raw.
    struct ElfHeader
    {
        std::uint32_t name {};
        std::uint32_t type {};
        std::uint64_t flags {};
        std::uint64_t offset {};
        std::uint64_t size {};
        std::uint32_t link {};
    };

    [[nodiscard]] std::optional<ElfHeader> ReadElfHeader(FieldReader const& reader,
                                                         ElfClass const& elf,
                                                         std::size_t at) noexcept
    {
        auto const name = reader.At<std::uint32_t>(at);
        auto const type = reader.At<std::uint32_t>(at + 4);
        auto const flags = WordAt(reader, elf.wordWidth, at + elf.flagsAt);
        auto const offset = WordAt(reader, elf.wordWidth, at + elf.offsetAt);
        auto const size = WordAt(reader, elf.wordWidth, at + elf.sizeAt);
        auto const link = reader.At<std::uint32_t>(at + elf.linkAt);
        if (!name || !type || !flags || !offset || !size || !link)
            return std::nullopt;
        return ElfHeader { .name = *name, .type = *type, .flags = *flags, .offset = *offset, .size = *size, .link = *link };
    }

    [[nodiscard]] std::optional<ObjectSections> WalkElf(std::span<std::byte const> image)
    {
        constexpr std::size_t IdentClassAt = 4;
        constexpr std::size_t IdentDataAt = 5;
        if (image.size() < 16)
            return std::nullopt;
        auto const classByte = std::to_integer<std::uint8_t>(image[IdentClassAt]);
        auto const dataByte = std::to_integer<std::uint8_t>(image[IdentDataAt]);
        if ((classByte != 1 && classByte != 2) || (dataByte != 1 && dataByte != 2))
            return std::nullopt;
        auto const& elf = classByte == 1 ? Elf32 : Elf64;
        auto const reader = FieldReader { .image = image, .bigEndian = dataByte == 2 };

        auto const tableAt = WordAt(reader, elf.sectionTableWidth, elf.sectionTableAt);
        auto const entrySize = reader.At<std::uint16_t>(elf.sectionEntrySizeAt);
        auto const declaredCount = reader.At<std::uint16_t>(elf.sectionCountAt);
        auto const declaredNames = reader.At<std::uint16_t>(elf.namesIndexAt);
        if (!tableAt || !entrySize || !declaredCount || !declaredNames || *tableAt == 0 || *entrySize < elf.entrySize)
            return std::nullopt;

        // Extended numbering: an object with more sections than a 16-bit field holds --
        // `-ffunction-sections` over a template-heavy TU reaches that -- stores the real
        // count in section 0's `sh_size` and the name table's index in its `sh_link`.
        auto const first = ReadElfHeader(reader, elf, static_cast<std::size_t>(*tableAt));
        if (!first.has_value())
            return std::nullopt;
        auto const count = *declaredCount != 0 ? std::uint64_t { *declaredCount } : first->size;
        auto const namesIndex = *declaredNames != ElfExtendedIndex ? std::uint64_t { *declaredNames } : first->link;
        // Bounded BEFORE the multiplication, so a count read out of section 0 cannot
        // wrap the product into a range that fits.
        if (count > image.size() / *entrySize || !Fits(*tableAt, count * *entrySize, image.size()) || namesIndex >= count)
            return std::nullopt;

        auto const headerAt = [&](std::uint64_t index) {
            return static_cast<std::size_t>(*tableAt + (index * *entrySize));
        };
        auto const names = ReadElfHeader(reader, elf, headerAt(namesIndex));
        if (!names.has_value() || !Fits(names->offset, names->size, image.size()))
            return std::nullopt;
        auto const nameTable = image.subspan(static_cast<std::size_t>(names->offset), static_cast<std::size_t>(names->size));

        ObjectSections laidOut { .format = ObjectFormat::Elf, .sections = {} };
        laidOut.sections.reserve(static_cast<std::size_t>(count));
        for (auto const index: std::views::iota(std::uint64_t { 0 }, count))
        {
            auto const header = ReadElfHeader(reader, elf, headerAt(index));
            if (!header.has_value())
                return std::nullopt;
            if (header->type == ElfSectionNoBits || header->size == 0)
                continue;
            if (!Fits(header->offset, header->size, image.size()))
                return std::nullopt;
            auto name = TerminatedName(nameTable, header->name);
            if (!name.has_value())
                return std::nullopt;
            laidOut.sections.push_back({ .name = std::move(*name),
                                         .at = static_cast<std::size_t>(header->offset),
                                         .size = static_cast<std::size_t>(header->size),
                                         .compressed = (header->flags & ElfSectionCompressed) != 0 });
        }
        return laidOut;
    }

    // --- Mach-O ----------------------------------------------------------------

    /// Where the fields of one Mach-O word size live.
    struct MachOClass
    {
        std::uint32_t magic {};          ///< The little-endian magic.
        std::size_t headerSize {};       ///< `mach_header` / `mach_header_64`.
        std::uint32_t segmentCommand {}; ///< `LC_SEGMENT` / `LC_SEGMENT_64`.
        std::size_t segmentSize {};      ///< The segment command up to its sections.
        std::size_t sectionCountAt {};   ///< `nsects`, within the segment command.
        std::size_t sectionSize {};      ///< `section` / `section_64`.
        std::size_t wordWidth {};        ///< Width of a section's `size`.
        std::size_t sizeAt {};           ///< `size`, within a section.
        std::size_t offsetAt {};         ///< `offset`, within a section.
        std::size_t flagsAt {};          ///< `flags`, within a section.
    };

    constexpr MachOClass MachO32 { .magic = 0xFEEDFACE,
                                   .headerSize = 28,
                                   .segmentCommand = 0x1,
                                   .segmentSize = 56,
                                   .sectionCountAt = 48,
                                   .sectionSize = 68,
                                   .wordWidth = 4,
                                   .sizeAt = 36,
                                   .offsetAt = 40,
                                   .flagsAt = 56 };

    constexpr MachOClass MachO64 { .magic = 0xFEEDFACF,
                                   .headerSize = 32,
                                   .segmentCommand = 0x19,
                                   .segmentSize = 72,
                                   .sectionCountAt = 64,
                                   .sectionSize = 80,
                                   .wordWidth = 8,
                                   .sizeAt = 40,
                                   .offsetAt = 48,
                                   .flagsAt = 64 };

    /// The section types that occupy no bytes in the file: `S_ZEROFILL`,
    /// `S_GB_ZEROFILL`, `S_THREAD_LOCAL_ZEROFILL`.
    constexpr std::array<std::uint32_t, 3> MachOZeroFillTypes { 0x1, 0xC, 0x12 };

    /// Walk one thin Mach-O image starting at @p base, appending its sections.
    /// @return False when the image cannot be laid out completely.
    [[nodiscard]] bool WalkThinMachO(std::span<std::byte const> image,
                                     std::size_t base,
                                     std::size_t extent,
                                     std::vector<ObjectSection>& sections)
    {
        if (!Fits(base, extent, image.size()))
            return false;
        auto const slice = image.subspan(base, extent);
        auto const reader = LittleEndian(slice);
        auto const magic = reader.At<std::uint32_t>(0);
        if (!magic.has_value())
            return false;
        MachOClass const* macho = nullptr;
        if (*magic == MachO32.magic)
            macho = &MachO32;
        else if (*magic == MachO64.magic)
            macho = &MachO64;
        else
            return false;

        auto const commands = reader.At<std::uint32_t>(16);
        if (!commands.has_value())
            return false;
        auto cursor = macho->headerSize;
        for ([[maybe_unused]] auto const command: std::views::iota(std::uint32_t { 0 }, *commands))
        {
            auto const kind = reader.At<std::uint32_t>(cursor);
            auto const length = reader.At<std::uint32_t>(cursor + 4);
            if (!kind.has_value() || !length.has_value() || *length < 8 || !Fits(cursor, *length, slice.size()))
                return false;
            if (*kind == macho->segmentCommand)
            {
                auto const count = reader.At<std::uint32_t>(cursor + macho->sectionCountAt);
                if (!count.has_value() || macho->segmentSize + (std::size_t { *count } * macho->sectionSize) > *length)
                    return false;
                for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { *count }))
                {
                    auto const at = cursor + macho->segmentSize + (index * macho->sectionSize);
                    auto const size = WordAt(reader, macho->wordWidth, at + macho->sizeAt);
                    auto const offset = reader.At<std::uint32_t>(at + macho->offsetAt);
                    auto const flags = reader.At<std::uint32_t>(at + macho->flagsAt);
                    if (!size.has_value() || !offset.has_value() || !flags.has_value())
                        return false;
                    if (std::ranges::find(MachOZeroFillTypes, *flags & 0xFFU) != MachOZeroFillTypes.end() || *size == 0)
                        continue;
                    if (!Fits(*offset, *size, slice.size()))
                        return false;
                    // `segment,section`, the spelling `ld` and `otool` use.
                    auto name = FixedName(slice.subspan(at + 16, 16));
                    name += ',';
                    name += FixedName(slice.subspan(at, 16));
                    sections.push_back({ .name = std::move(name),
                                         .at = base + static_cast<std::size_t>(*offset),
                                         .size = static_cast<std::size_t>(*size),
                                         .compressed = false });
                }
            }
            cursor += *length;
        }
        return true;
    }

    [[nodiscard]] std::optional<ObjectSections> WalkMachO(std::span<std::byte const> image)
    {
        ObjectSections laidOut { .format = ObjectFormat::MachO, .sections = {} };

        // A universal file: a big-endian table of slices, each a thin image. This is
        // what `-arch arm64 -arch x86_64` -- CMAKE_OSX_ARCHITECTURES with two entries --
        // hands a compile cache.
        auto const table = FieldReader { .image = image, .bigEndian = true };
        auto const fatMagic = table.At<std::uint32_t>(0);
        constexpr std::uint32_t Fat32 = 0xCAFEBABE;
        constexpr std::uint32_t Fat64 = 0xCAFEBABF;
        if (fatMagic == Fat32 || fatMagic == Fat64)
        {
            auto const wide = *fatMagic == Fat64;
            auto const entrySize = wide ? std::size_t { 32 } : std::size_t { 20 };
            auto const slices = table.At<std::uint32_t>(4);
            if (!slices.has_value() || !Fits(8, std::uint64_t { *slices } * entrySize, image.size()))
                return std::nullopt;
            for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { *slices }))
            {
                auto const at = 8 + (index * entrySize);
                auto const offset =
                    wide ? table.At<std::uint64_t>(at + 8)
                         : table.At<std::uint32_t>(at + 8).transform([](auto v) { return std::uint64_t { v }; });
                auto const size =
                    wide ? table.At<std::uint64_t>(at + 16)
                         : table.At<std::uint32_t>(at + 12).transform([](auto v) { return std::uint64_t { v }; });
                if (!offset || !size || !Fits(*offset, *size, image.size())
                    || !WalkThinMachO(
                        image, static_cast<std::size_t>(*offset), static_cast<std::size_t>(*size), laidOut.sections))
                    return std::nullopt;
            }
            return laidOut;
        }

        if (!WalkThinMachO(image, 0, image.size(), laidOut.sections))
            return std::nullopt;
        return laidOut;
    }
} // namespace

bool HasBigObjSignature(std::span<std::byte const> image) noexcept
{
    auto const reader = LittleEndian(image);
    auto const sig1 = reader.At<std::uint16_t>(0);
    auto const sig2 = reader.At<std::uint16_t>(2);
    return sig1.has_value() && sig2.has_value() && *sig1 == 0 && *sig2 == BigObjSignature;
}

std::optional<std::uint64_t> CoffSectionCount(CoffLayout const& layout, std::span<std::byte const> image) noexcept
{
    // One spelling, because the width is the whole difference between the two layouts
    // here and a second copy of the ternary is a second place to get it wrong. Returned
    // as the wide type: a `/bigobj` count is a full 32 bits.
    if (layout.sectionCountWidth == sizeof(std::uint16_t))
        return LittleEndian(image).At<std::uint16_t>(layout.sectionCountAt);
    return LittleEndian(image).At<std::uint32_t>(layout.sectionCountAt);
}

bool IsConsistentCoff(CoffLayout const& layout, std::span<std::byte const> image) noexcept
{
    if (image.size() < layout.headerSize)
        return false;
    auto const reader = LittleEndian(image);
    auto const sections = CoffSectionCount(layout, image);
    auto const symbolPointer = reader.At<std::uint32_t>(layout.symbolPointerAt);
    auto const symbolCount = reader.At<std::uint32_t>(layout.symbolCountAt);
    auto const tableAt = CoffSectionTableAt(layout, image);
    if (!sections.has_value() || !symbolPointer.has_value() || !symbolCount.has_value() || !tableAt.has_value())
        return false;
    if (*tableAt + (CoffSectionHeaderSize * *sections) > image.size())
        return false;
    // A symbol table pointer of zero means there is none, which is legal and is not a
    // claim about the file's size.
    if (*symbolPointer == 0)
        return *symbolCount == 0;
    return *symbolPointer + (layout.symbolRecordSize * *symbolCount) <= image.size();
}

CoffLayout const* ChooseCoffLayout(std::span<std::byte const> image) noexcept
{
    if (IsKnownNonCoff(image))
        return nullptr;
    if (HasBigObjSignature(image))
    {
        auto const version = LittleEndian(image).At<std::uint16_t>(4);
        if (!version.has_value() || *version < BigObjMinimumVersion || !IsConsistentCoff(BigObjCoff, image))
            return nullptr;
        return &BigObjCoff;
    }
    return IsConsistentCoff(StandardCoff, image) ? &StandardCoff : nullptr;
}

std::vector<ObjectSection> CoffSections(CoffLayout const& layout, std::span<std::byte const> image)
{
    return WalkCoff(layout, image).sections;
}

std::optional<ObjectSections> ReadObjectSections(std::span<std::byte const> image)
{
    constexpr std::array<std::byte, 4> ElfMagic {
        std::byte { 0x7F }, std::byte { 'E' }, std::byte { 'L' }, std::byte { 'F' }
    };
    if (image.size() >= ElfMagic.size() && std::ranges::equal(image.first(ElfMagic.size()), ElfMagic))
        return WalkElf(image);
    if (IsKnownNonCoff(image))
        return WalkMachO(image);
    auto const* layout = ChooseCoffLayout(image);
    if (layout == nullptr)
        return std::nullopt;
    auto walk = WalkCoff(*layout, image);
    if (!walk.complete)
        return std::nullopt;
    return ObjectSections { .format = ObjectFormat::Coff, .sections = std::move(walk.sections) };
}

} // namespace FastCache::Cc
