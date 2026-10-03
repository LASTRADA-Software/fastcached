// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace FastCache::Cc
{

/// Walking an object file's sections, in ONE place, for the two readers that need it.
///
/// `ObjectEquivalence` walks a COFF object to say WHERE two objects differ, and
/// `RootBinding` walks every format this cache stores to decide which bytes a linked
/// program can see. The COFF half used to live privately inside the first; the second
/// needed exactly that walk plus ELF and Mach-O, and a second copy of one layout is a
/// copy that drifts (`StubCoffTestSupport.hpp` records two that already had).
///
/// **Nothing here decides anything.** A section is reported by name, extent and the
/// one flag a reader cannot recover from the name (`compressed`); which of them are
/// debug records, which are program data and which cannot be read as bytes at all is
/// the reader's policy, kept where the reader keeps its reasons.

/// One COFF header layout: where the fields a reader needs live.
///
/// A table because there are two of these and MSVC picks between them without being
/// asked: `/bigobj` is an allowed argument (`CompileJob.cpp`), so such a build is
/// cacheable and reaches both readers, and a layout row is what keeps it from getting
/// a permanent "cannot read" nobody would investigate. A third layout is a row.
///
/// Not keyed on the compiler `Flavor`: `/bigobj` is chosen per INVOCATION rather than
/// per driver, and a served image may have been produced by another driver on another
/// machine -- so the launcher's own flavour is not an answer about the bytes in hand.
/// Only the image can say, which is what `ChooseCoffLayout` asks it.
struct CoffLayout
{
    /// Bytes the fixed header occupies -- and therefore where the section table
    /// starts. One field rather than two: the section table begins where the header
    /// ends, so a second spelling could only ever disagree.
    std::size_t headerSize {};
    std::size_t timeDateStampAt {};   ///< Offset of the clock.
    std::size_t sectionCountAt {};    ///< Offset of `NumberOfSections`.
    std::size_t sectionCountWidth {}; ///< Its width -- two bytes, four on `/bigobj`.
    std::size_t symbolPointerAt {};   ///< Offset of `PointerToSymbolTable`.
    std::size_t symbolCountAt {};     ///< Offset of `NumberOfSymbols`.
    std::size_t symbolRecordSize {};  ///< 18 bytes, 20 on `/bigobj`.
    /// Offset of `SizeOfOptionalHeader`, or nothing where the layout has no optional
    /// header at all -- which is a different fact from one of size zero.
    std::optional<std::size_t> optionalHeaderSizeAt {};
};

/// Whether @p image carries a `/bigobj` signature, whatever its version.
///
/// Separate from laying it out, because the two answers differ: a signature this
/// recognises and a version it does not is an MSVC object this code cannot read --
/// an LTCG (`/GL`) object is the ordinary one -- while no signature at all just means
/// "some other format".
/// @param image The bytes to test.
/// @return True when `Sig1` is zero and `Sig2` is 0xFFFF.
[[nodiscard]] bool HasBigObjSignature(std::span<std::byte const> image) noexcept;

/// How @p image should be read as COFF, if this code knows how.
/// @param image The bytes.
/// @return The layout, or null when there is none to apply -- which covers both
///         "some other format" and an MSVC object this cannot lay out. The two are
///         told apart by asking `HasBigObjSignature`, because only a `/bigobj`
///         signature can be recognised and still not laid out.
[[nodiscard]] CoffLayout const* ChooseCoffLayout(std::span<std::byte const> image) noexcept;

/// Whether @p image is internally consistent when read by @p layout: everything the
/// header CLAIMS is present actually fits inside the file.
/// @param layout The layout to read by.
/// @param image The bytes.
/// @return True when the section table and symbol table lie inside @p image.
[[nodiscard]] bool IsConsistentCoff(CoffLayout const& layout, std::span<std::byte const> image) noexcept;

/// `NumberOfSections`, read at this layout's width.
/// @param layout The layout to read by.
/// @param image The bytes.
/// @return The count, or nothing when the field does not fit.
[[nodiscard]] std::optional<std::uint64_t> CoffSectionCount(CoffLayout const& layout,
                                                            std::span<std::byte const> image) noexcept;

/// The object formats the walk can lay out.
///
/// Private: never transmitted or persisted, so no enumerator carries an explicit value.
enum class ObjectFormat : std::uint8_t
{
    Coff,  ///< Both COFF header layouts.
    Elf,   ///< ELF32 and ELF64, either byte order.
    MachO, ///< Thin 32- and 64-bit little-endian Mach-O, and a universal file of them.
};

/// One section, as the walk sees it.
struct ObjectSection
{
    /// The section's name. A COFF long name is resolved through the string table; a
    /// Mach-O section is `segment,section` -- `__DWARF,__debug_info` -- because the
    /// segment is what says what the section is for.
    std::string name;
    std::size_t at {};   ///< Offset of the section's data in the image.
    std::size_t size {}; ///< Length of that data.
    /// ELF `SHF_COMPRESSED`: the bytes are a compressed stream, so nothing in them can
    /// be read as the text it encodes. Never set for the other formats, which have no
    /// such flag.
    bool compressed { false };
};

/// A laid-out object.
struct ObjectSections
{
    ObjectFormat format { ObjectFormat::Coff };
    /// Every section that has DATA in the file, in file order. A section with none --
    /// `.bss`, an ELF `SHT_NOBITS`, a Mach-O zero-fill -- holds no bytes to read and is
    /// not listed.
    std::vector<ObjectSection> sections;
};

/// The sections of a COFF image read by @p layout, whose data lies inside the image.
///
/// Lenient, for `ObjectEquivalence`, whose walk is purely descriptive: a section whose
/// data does not fit is left out rather than failing the walk, so a parser defect can
/// make a message vague and cannot change a verdict.
/// @param layout The layout to read by.
/// @param image The bytes.
/// @return The sections; empty when the table cannot be followed.
[[nodiscard]] std::vector<ObjectSection> CoffSections(CoffLayout const& layout, std::span<std::byte const> image);

/// Lay out @p image as whichever format it is.
///
/// **Strict, unlike `CoffSections`**: a section claiming data that does not fit, or a
/// table that cannot be followed, is no layout at all. A reader deciding what a program
/// can see must not be handed a partial list and take it for the whole one.
/// @param image The bytes.
/// @return The sections, or nothing when the image is not a format this walk can lay
///         out completely.
[[nodiscard]] std::optional<ObjectSections> ReadObjectSections(std::span<std::byte const> image);

} // namespace FastCache::Cc
