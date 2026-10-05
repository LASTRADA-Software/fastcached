// SPDX-License-Identifier: Apache-2.0
#include "KeyDigest.hpp"
#include "ObjectSections.hpp"
#include "RootBinding.hpp"
#include "RootReconciler.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <utility>

#include <core/Ranges.hpp>

namespace FastCache::Cc
{

namespace
{
    /// What every non-ASCII run becomes, in a root and in the bytes scanned alike -- and
    /// what a `?` becomes too.
    ///
    /// A run of non-ASCII in the root matches a run of non-ASCII in the object, whatever
    /// encoding either was written in. A `?` joins the class because `cl` without `/utf-8`
    /// writes one for each root character its code page cannot hold (measured: a checkout
    /// under `k检-aaa`, ACP 1252, gave `k?-aaa` in `.rdata`), and IN THE OBJECT the wildcard
    /// matches any one unit of a root (`Matches`). Both are the broad side, which is the
    /// side this scan must take.
    constexpr char Wildcard = static_cast<char>(0x80);

    /// Fold one code unit into the comparison alphabet: ASCII case folded, both
    /// separators one character, everything non-ASCII -- and `?` -- the wildcard.
    [[nodiscard]] constexpr char Fold(std::uint32_t unit) noexcept
    {
        if (unit >= 0x80U || unit == '?')
            return Wildcard;
        auto const c = static_cast<char>(unit);
        if (c == '\\')
            return '/';
        if (c >= 'A' && c <= 'Z')
            return static_cast<char>(c - 'A' + 'a');
        return c;
    }

    /// Append @p folded, collapsing a run of separators, and a run of wildcards, into
    /// one -- `\\` and a multi-byte character each read as a single unit.
    void Append(std::string& out, char folded)
    {
        if ((folded == '/' || folded == Wildcard) && !out.empty() && out.back() == folded)
            return;
        out.push_back(folded);
    }

    /// A root in the comparison alphabet. Its bytes are UTF-8 -- `argv` and the
    /// environment are, on every host this builds for -- so every multi-byte character
    /// is a run of bytes at or above 0x80, and folds to one wildcard.
    [[nodiscard]] std::string FoldRoot(std::string_view root)
    {
        std::string folded;
        folded.reserve(root.size());
        for (auto const c: root)
            Append(folded, Fold(static_cast<unsigned char>(c)));
        return folded;
    }

    /// One way a string may be encoded in an object.
    struct Encoding
    {
        std::string_view name;
        std::size_t unitWidth {};
    };

    /// Every encoding scanned, each at every alignment its unit width allows. A new
    /// encoding is a row.
    constexpr std::array<Encoding, 3> Encodings { {
        { .name = "narrow", .unitWidth = 1 },
        { .name = "UTF-16LE", .unitWidth = 2 },
        { .name = "UTF-32LE", .unitWidth = 4 },
    } };

    /// Fold @p bytes read as little-endian units of @p width starting at @p alignment.
    /// @param out Receives the folded text; cleared first, so one buffer serves every pass.
    void FoldUnits(std::span<std::byte const> bytes, std::size_t width, std::size_t alignment, std::string& out)
    {
        out.clear();
        if (alignment >= bytes.size())
            return;
        auto const units = (bytes.size() - alignment) / width;
        out.reserve(units);
        for (auto const index: std::views::iota(std::size_t { 0 }, units))
        {
            auto const unit = bytes.subspan(alignment + (index * width), width);
            std::uint32_t value = 0;
            for (auto const shift: std::views::iota(std::size_t { 0 }, width))
                value |= std::to_integer<std::uint32_t>(unit[shift]) << (8U * shift);
            Append(out, Fold(value));
        }
    }

    /// What role a section plays for this question.
    enum class SectionRole : std::uint8_t
    {
        /// Bytes a linked program can see, or a linker acts on. Scanned.
        Program,
        /// A record of WHERE the compile happened -- debug info, the COFF checksum.
        /// #203's accepted cost, and never scanned; see this file's header.
        Debug,
        /// Content that cannot be read as the bytes it encodes. The object is bound.
        Opaque,
        /// Fixed-size records a linker merges or an unwinder walks -- constant and string
        /// pools, unwind tables. Scanned as program data, but never taken for a compressed
        /// payload by the bytes it opens with: a pool is many independent entries and a
        /// table is structured, so neither can BE one stream, and the first entry is
        /// whatever constant the source happened to hold.
        Records,
    };

    /// What must follow a rule's prefix for a section name to match it.
    enum class NameTail : std::uint8_t
    {
        /// Anything, the empty tail included.
        Any,
        /// A decimal digit: the entry size a toolchain appends to a MERGED pool
        /// (`.rodata.str1.1`, `.rodata.cst8`). Under `-fdata-sections` a variable named
        /// `str_blob` gets `.rodata.str_blob` -- the same prefix, and not a pool at all.
        Digit,
    };

    /// A section-name prefix and the role it declares.
    struct SectionRule
    {
        std::string_view prefix;
        SectionRole role {};
        NameTail tail { NameTail::Any };
    };

    /// @param name A section name. @param rule A rule. @return Whether @p name matches it.
    [[nodiscard]] constexpr bool Matches(std::string_view name, SectionRule const& rule) noexcept
    {
        if (!name.starts_with(rule.prefix))
            return false;
        switch (rule.tail)
        {
            case NameTail::Any:
                return true;
            case NameTail::Digit:
                return name.size() > rule.prefix.size() && name[rule.prefix.size()] >= '0'
                       && name[rule.prefix.size()] <= '9';
        }
        return false;
    }

    /// Which sections are NOT program data, by name.
    ///
    /// The excluded set is named rather than the scanned set, and the direction is the
    /// point: a section nobody anticipated is scanned, which costs at most a miss. Not
    /// keyed on the format, because no two formats spell these names alike -- a COFF
    /// object cannot carry a `__DWARF,` section, nor ELF a `.chks64`.
    constexpr std::array<SectionRule, 24> SectionRules { {
        // COFF CodeView (`.debug$S`, `.debug$T`, ...) and DWARF in every format that
        // names it with a leading dot.
        { .prefix = ".debug", .role = SectionRole::Debug },
        // ELF's compressed-DWARF spelling, and the relocations against debug sections.
        { .prefix = ".zdebug", .role = SectionRole::Debug },
        { .prefix = ".rel.debug", .role = SectionRole::Debug },
        { .prefix = ".rela.debug", .role = SectionRole::Debug },
        // Separate-debug-file links, and the debug info GCC writes beside LTO IR.
        { .prefix = ".gnu_debug", .role = SectionRole::Debug },
        { .prefix = ".gnu.debuglto_", .role = SectionRole::Debug },
        // `cl`'s checksum of the object, which moves with the path records it covers.
        { .prefix = ".chks64", .role = SectionRole::Debug },
        // Mach-O's debug segment.
        { .prefix = "__DWARF,", .role = SectionRole::Debug },
        // LTO intermediate code and embedded bitcode: GCC's IR is compressed, and LLVM
        // bitcode packs strings below byte granularity, so neither can be read here.
        { .prefix = ".gnu.lto_", .role = SectionRole::Opaque },
        { .prefix = ".llvm.lto", .role = SectionRole::Opaque },
        { .prefix = ".llvmbc", .role = SectionRole::Opaque },
        { .prefix = "__LLVM,", .role = SectionRole::Opaque },
        // clang's coverage map, in each format's spelling: a zlib stream behind a header,
        // carrying the compilation directory and the absolute source. Measured on clang 22
        // under `-fcoverage-mapping` (`__llvm_covmap`): the root appears nowhere in plain
        // bytes. The Windows spelling is uncompressed in VS's clang-cl, and opaque anyway --
        // whether it is compressed is a property of the build, not of the format.
        { .prefix = "__llvm_cov", .role = SectionRole::Opaque },
        { .prefix = ".lcovmap$", .role = SectionRole::Opaque },
        { .prefix = ".lcovfun$", .role = SectionRole::Opaque },
        { .prefix = "__LLVM_COV,", .role = SectionRole::Opaque },
        // Device images: offloaded code and CUDA fat binaries, possibly compressed, which
        // can carry a device-side `assert`'s `__FILE__`. NOT measured -- listed because the
        // wrong answer about them is the unsafe one.
        { .prefix = ".llvm.offloading", .role = SectionRole::Opaque },
        { .prefix = ".nv_fatbin", .role = SectionRole::Opaque },
        // Sections whose opening bytes say nothing about compression. Each was measured
        // tripping the compressed-content rule on real objects: `cl` Release `.pdata`
        // tables opening `78 01` (2 of 410 objects), and an ELF `.rodata.cst8` holding the
        // literal `"\x78\x9C..."` -- the first entry of a merged pool is whatever constant
        // the source wrote, and it kept one test unit from replaying in a second build
        // directory. Mach-O's pools are the same objects under their own names.
        { .prefix = ".pdata", .role = SectionRole::Records },
        { .prefix = ".xdata", .role = SectionRole::Records },
        { .prefix = ".rodata.cst", .role = SectionRole::Records, .tail = NameTail::Digit },
        { .prefix = ".rodata.str", .role = SectionRole::Records, .tail = NameTail::Digit },
        { .prefix = "__TEXT,__cstring", .role = SectionRole::Records },
        { .prefix = "__TEXT,__literal", .role = SectionRole::Records },
    } };

    /// A compression format recognised by the bytes a section OPENS with.
    struct CompressedHeader
    {
        std::array<std::byte, 4> magic {};
        std::size_t length {};
        std::string_view name;
    };

    /// The headers that make a section opaque whatever it is called.
    ///
    /// Narrow on purpose, because this is asked of every section's first bytes and a
    /// loose rule binds everything: zlib's two-byte header matched by its checksum alone
    /// accepts `48 89`, which is how most x86-64 functions begin. So zlib is the one
    /// window size a compressor writes (`0x78`) with each of its four check-valid levels,
    /// and zstd is its full four-byte frame magic. A stream behind a header of its own --
    /// clang's coverage map -- is caught by name in `SectionRules` instead.
    constexpr std::array<CompressedHeader, 5> CompressedHeaders { {
        { .magic = { std::byte { 0x78 }, std::byte { 0x01 } }, .length = 2, .name = "a zlib stream" },
        { .magic = { std::byte { 0x78 }, std::byte { 0x5E } }, .length = 2, .name = "a zlib stream" },
        { .magic = { std::byte { 0x78 }, std::byte { 0x9C } }, .length = 2, .name = "a zlib stream" },
        { .magic = { std::byte { 0x78 }, std::byte { 0xDA } }, .length = 2, .name = "a zlib stream" },
        { .magic = { std::byte { 0x28 }, std::byte { 0xB5 }, std::byte { 0x2F }, std::byte { 0xFD } },
          .length = 4,
          .name = "a zstd frame" },
    } };

    /// @param bytes A section's contents. @return The compression it opens with, if any.
    [[nodiscard]] std::optional<std::string_view> CompressedContent(std::span<std::byte const> bytes) noexcept
    {
        for (auto const& header: CompressedHeaders)
            if (bytes.size() >= header.length
                && std::ranges::equal(bytes.first(header.length), std::span { header.magic }.first(header.length)))
                return header.name;
        return std::nullopt;
    }

    /// @param section One section. @param bytes Its contents. @return Its role for this scan.
    [[nodiscard]] SectionRole RoleOf(ObjectSection const& section, std::span<std::byte const> bytes) noexcept
    {
        auto const* const rule = core::findIfOrNull(
            SectionRules, [&section](SectionRule const& candidate) { return Matches(section.name, candidate); });
        auto const role = rule == nullptr ? SectionRole::Program : rule->role;
        switch (role)
        {
            case SectionRole::Debug:
            case SectionRole::Opaque:
                return role;
            case SectionRole::Records:
                // The flag is the format's own word, and a pool compressed as a whole
                // hides its strings like anything else.
                return section.compressed ? SectionRole::Opaque : SectionRole::Program;
            case SectionRole::Program:
                break;
        }
        // Compressed content hides its strings the way LTO IR does: flagged so, or opening
        // with a compressor's header.
        if (section.compressed || CompressedContent(bytes).has_value())
            return SectionRole::Opaque;
        return SectionRole::Program;
    }

    /// A whole-image format that cannot be read as the bytes it encodes, recognised by
    /// its leading bytes.
    struct OpaqueImage
    {
        std::array<std::byte, 4> magic {};
        std::string_view name;
    };

    /// The `-flto` outputs a walk cannot lay out: raw LLVM bitcode, and the wrapper
    /// Darwin puts around it. `cl /GL`'s anonymous object is recognised structurally
    /// instead, by `HasBigObjSignature` on an image no COFF layout accepts.
    constexpr std::array<OpaqueImage, 2> OpaqueImages { {
        { .magic = { std::byte { 'B' }, std::byte { 'C' }, std::byte { 0xC0 }, std::byte { 0xDE } },
          .name = "LLVM bitcode" },
        { .magic = { std::byte { 0xDE }, std::byte { 0xC0 }, std::byte { 0x17 }, std::byte { 0x0B } },
          .name = "wrapped LLVM bitcode" },
    } };

    /// @param image The object. @return The name of the opaque format it is, if it is one.
    [[nodiscard]] std::optional<std::string_view> OpaqueImageName(std::span<std::byte const> image) noexcept
    {
        for (auto const& opaque: OpaqueImages)
            if (image.size() >= opaque.magic.size() && std::ranges::equal(image.first(opaque.magic.size()), opaque.magic))
                return opaque.name;
        if (HasBigObjSignature(image) && ChooseCoffLayout(image) == nullptr)
            return "an MSVC anonymous object (cl /GL)";
        return std::nullopt;
    }

    /// Whether @p root occurs in @p scanned, an object's wildcard matching any one unit of
    /// the root -- a substituted `?` stands for whatever character the code page lost.
    /// @param scanned Folded object bytes.
    /// @param root A folded root.
    [[nodiscard]] bool Matches(std::string_view scanned, std::string_view root)
    {
        if (!scanned.contains(Wildcard))
            return scanned.contains(root);
        return !std::ranges::search(scanned, root, [](char object, char wanted) {
                    return object == wanted || object == Wildcard;
                }).empty();
    }

    /// One spelling to look for, and the parts of the checkout it spells -- a working
    /// directory that IS the build tree is one spelling of two parts.
    struct Needle
    {
        std::string spelling;
        std::string folded;
        std::uint8_t parts {};
    };

    /// What a scan has found so far: the parts, as bits, and the evidence for the first.
    struct Findings
    {
        std::uint8_t parts {};
        std::string evidence;
    };

    /// Search one region for every needle whose parts are not all found yet.
    /// @param bytes The region.
    /// @param where What to call it in the evidence.
    /// @param needles What to look for.
    /// @param wanted Every part bit the needles can report; the search stops once all are found.
    /// @param buffer Scratch space, reused across regions.
    /// @param found Accumulates the parts found, and the evidence for the first.
    void FindParts(std::span<std::byte const> bytes,
                   std::string_view where,
                   std::span<Needle const> needles,
                   std::uint8_t wanted,
                   std::string& buffer,
                   Findings& found)
    {
        for (auto const& encoding: Encodings)
        {
            for (auto const alignment: std::views::iota(std::size_t { 0 }, encoding.unitWidth))
            {
                if (found.parts == wanted)
                    return;
                FoldUnits(bytes, encoding.unitWidth, alignment, buffer);
                for (auto const& needle: needles)
                {
                    if ((found.parts | needle.parts) == found.parts || !Matches(buffer, needle.folded))
                        continue;
                    found.parts |= needle.parts;
                    if (found.evidence.empty())
                        found.evidence = std::format("names {} in {} ({})", needle.spelling, where, encoding.name);
                }
            }
        }
    }

    /// Scan @p image for every needle.
    /// @param image The object file.
    /// @param needles What to look for.
    /// @param opaqueParts The part bits an object this scan cannot read is bound to.
    /// @return The verdict, its evidence, and the part bits found.
    [[nodiscard]] std::pair<RootBindingScan, std::uint8_t> ScanNeedles(std::span<std::byte const> image,
                                                                       std::span<Needle const> needles,
                                                                       std::uint8_t opaqueParts)
    {
        auto const unreadable = [opaqueParts](std::string evidence) {
            return std::pair {
                RootBindingScan { .binding = RootBinding::Bound, .evidence = std::move(evidence), .parts = {} }, opaqueParts
            };
        };
        if (auto const opaque = OpaqueImageName(image))
            return unreadable(std::format("is {}, whose strings this scan cannot read", *opaque));

        std::uint8_t wanted = 0;
        for (auto const& needle: needles)
            wanted |= needle.parts;

        Findings found;
        std::string buffer;
        auto const laidOut = ReadObjectSections(image);
        if (!laidOut.has_value())
        {
            // A format the walk cannot lay out is scanned WHOLE: header, symbol tables and
            // any debug records included. Broad, so it can only cost a miss.
            FindParts(image, "an object this scan cannot lay out", needles, wanted, buffer, found);
        }
        else
        {
            for (auto const& section: laidOut->sections)
            {
                auto const bytes = image.subspan(section.at, section.size);
                switch (RoleOf(section, bytes))
                {
                    case SectionRole::Debug:
                        continue;
                    case SectionRole::Opaque:
                        return unreadable(std::format("carries {}, whose strings this scan cannot read", section.name));
                    case SectionRole::Records:
                    case SectionRole::Program:
                        break;
                }
                FindParts(bytes, section.name, needles, wanted, buffer, found);
                if (found.parts == wanted)
                    break;
            }
        }
        if (found.parts == 0)
            return { RootBindingScan { .binding = RootBinding::Portable, .evidence = {}, .parts = {} }, 0 };
        return { RootBindingScan { .binding = RootBinding::Bound, .evidence = std::move(found.evidence), .parts = {} },
                 found.parts };
    }

    /// One part of a checkout: its name in a marker, and how its spellings are read.
    struct CheckoutPartRow
    {
        CheckoutPart part;
        /// The name a marker spells it by. `[a-z-]` only, so a list of them needs no quoting.
        std::string_view name;
        /// The part made absolute, as exported or as the process read it.
        std::string const& (*absolute)(CheckoutRoots const&);
        /// The same, as the filesystem resolves it -- the spelling a bound key folds.
        std::string const& (*resolved)(CheckoutRoots const&);
        /// The part as a compiler spawned here writes it. Scanned for, never folded: a
        /// root is written as it was exported, the working directory as `$PWD` spells it.
        std::string const& (*spelled)(CheckoutRoots const&);
    };

    /// Every part, in enumerator order. A new part is a row.
    constexpr EnumTable<CheckoutPart, CheckoutPartRow> CheckoutPartTable { {
        { .part = CheckoutPart::SourceRoot,
          .name = "source-root",
          .absolute = [](CheckoutRoots const& c) -> std::string const& { return c.exported.sourceRoot; },
          .resolved = [](CheckoutRoots const& c) -> std::string const& { return c.resolved.sourceRoot; },
          .spelled = [](CheckoutRoots const& c) -> std::string const& { return c.exported.sourceRoot; } },
        { .part = CheckoutPart::BuildTree,
          .name = "build-tree",
          .absolute = [](CheckoutRoots const& c) -> std::string const& { return c.exported.buildTree; },
          .resolved = [](CheckoutRoots const& c) -> std::string const& { return c.resolved.buildTree; },
          .spelled = [](CheckoutRoots const& c) -> std::string const& { return c.exported.buildTree; } },
        { .part = CheckoutPart::WorkingDirectory,
          .name = "working-directory",
          .absolute = [](CheckoutRoots const& c) -> std::string const& { return c.workingDirectory; },
          .resolved = [](CheckoutRoots const& c) -> std::string const& { return c.resolvedWorkingDirectory; },
          .spelled = [](CheckoutRoots const& c) -> std::string const& { return c.compilerWorkingDirectory; } },
    } };
    static_assert(RowsInEnumeratorOrder(CheckoutPartTable, &CheckoutPartRow::part));

    /// @param part A part. @return Its bit in a `CheckoutParts`.
    [[nodiscard]] constexpr std::uint8_t BitOf(CheckoutPart part) noexcept
    {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned>(part));
    }

    /// Every spelling of every part of @p checkout, one needle per distinct spelling.
    ///
    /// A spelling still relative -- a working directory the process could not read comes
    /// back as `.` -- names no checkout, and `.` would match nearly any object.
    [[nodiscard]] std::vector<Needle> NeedlesOf(CheckoutRoots const& checkout)
    {
        // Asked in the working directory's convention, as `AbsoluteAgainst` asks it.
        PathCanon::Layout const convention { .sourceRoot = checkout.workingDirectory, .buildTree = {} };
        std::vector<Needle> needles;
        auto const add = [&](std::string const& spelling, CheckoutPart part) {
            if (spelling.empty() || PathCanon::AnchorForLayout(spelling, convention) == PathCanon::Anchor::WorkingDirectory)
                return;
            for (auto& needle: needles)
            {
                if (needle.spelling == spelling)
                {
                    needle.parts = static_cast<std::uint8_t>(needle.parts | BitOf(part));
                    return;
                }
            }
            needles.push_back(Needle { .spelling = spelling, .folded = FoldRoot(spelling), .parts = BitOf(part) });
        };
        for (auto const& row: CheckoutPartTable)
        {
            add(row.absolute(checkout), row.part);
            add(row.resolved(checkout), row.part);
            add(row.spelled(checkout), row.part);
        }
        if (checkout.aliases != nullptr)
            for (auto const& alias: checkout.aliases->Entries())
                add(alias.spelling, alias.part);
        return needles;
    }

    /// What every marker's object blob begins with, whatever its version.
    ///
    /// A fixed ASCII string: no object format this cache stores begins with it, and the
    /// recogniser asks for zero text regions as well, which no stored object has.
    constexpr std::string_view MarkerPrefix = "fastcache-cc root-bound marker ";

    /// The version this build writes and reads, followed by a space and the part names,
    /// comma-separated. `v1` carried no parts: it bound every object to the whole checkout.
    constexpr std::string_view MarkerVersion = "v2";

} // namespace

std::string AbsoluteAgainst(std::string_view root, std::string_view workingDirectory)
{
    if (root.empty())
        return {};
    // Asked in the working directory's own convention: it is absolute, so it says whether
    // this is a Windows path, and a relative root carries nothing that could.
    PathCanon::Layout const convention { .sourceRoot = std::string { workingDirectory }, .buildTree = {} };
    if (PathCanon::AnchorForLayout(root, convention) != PathCanon::Anchor::WorkingDirectory)
        return std::string { root };
    auto const separator = PathCanon::IsWindowsLayout(convention) ? '\\' : '/';
    auto joined = std::string { workingDirectory };
    if (!joined.empty() && joined.back() != '/' && joined.back() != '\\')
        joined.push_back(separator);
    joined.append(root);
    return PathCanon::LexicallyNormal(joined);
}

CheckoutRoots BindCheckout(RootReconciler& reconciler,
                           std::string_view workingDirectory,
                           std::string_view compilerWorkingDirectory)
{
    auto const& exported = reconciler.Layout();
    auto& resolver = reconciler.Resolver();
    CheckoutRoots checkout;
    checkout.workingDirectory = std::string { workingDirectory };
    checkout.compilerWorkingDirectory = std::string { compilerWorkingDirectory };
    checkout.exported = PathCanon::Layout { .sourceRoot = AbsoluteAgainst(exported.sourceRoot, workingDirectory),
                                            .buildTree = AbsoluteAgainst(exported.buildTree, workingDirectory) };
    auto const resolve = [&resolver](std::string const& path) {
        return path.empty() ? std::string {} : resolver.ResolveDirectory(path);
    };
    checkout.resolved = PathCanon::Layout { .sourceRoot = resolve(checkout.exported.sourceRoot),
                                            .buildTree = resolve(checkout.exported.buildTree) };
    checkout.resolvedWorkingDirectory = resolve(checkout.workingDirectory);

    // Each part's 8.3 short form joins the alias list: a compiler handed `...\BUILDV~1`
    // writes it into the object, and the key resolves it to the long root. Asked of the
    // absolute spelling, and only of one that IS absolute -- a short form of a relative
    // path would name wherever this process happens to stand.
    PathCanon::Layout const convention { .sourceRoot = checkout.workingDirectory, .buildTree = {} };
    for (auto const& row: CheckoutPartTable)
    {
        auto const& absolute = row.absolute(checkout);
        if (absolute.empty() || PathCanon::AnchorForLayout(absolute, convention) != PathCanon::Anchor::Absolute)
            continue;
        if (auto shortened = resolver.ShortForm(absolute); shortened != absolute)
            reconciler.Aliases().Add(row.part, std::move(shortened));
    }
    checkout.aliases = &reconciler.Aliases();
    return checkout;
}

std::string BoundMissReason(CheckoutParts parts)
{
    return std::format(
        "the cached object names its {}; this compile's differs, or its copy was evicted; compiled this one's own",
        parts.Names());
}

std::string BoundMissQualifier(CheckoutParts parts)
{
    return std::format(" (root-bound: {})", BoundMissReason(parts));
}

std::string_view CheckoutPartName(CheckoutPart part) noexcept
{
    return CheckoutPartTable[static_cast<std::size_t>(part)].name;
}

CheckoutParts CheckoutParts::All() noexcept
{
    CheckoutParts all;
    for (auto const part: Enumerators<CheckoutPart>())
        all = all.With(part);
    return all;
}

CheckoutParts CheckoutParts::With(CheckoutPart part) const noexcept
{
    CheckoutParts with = *this;
    with._bits = static_cast<std::uint8_t>(with._bits | BitOf(part));
    return with;
}

bool CheckoutParts::Contains(CheckoutPart part) const noexcept
{
    return (_bits & BitOf(part)) != 0;
}

bool CheckoutParts::Empty() const noexcept
{
    return _bits == 0;
}

std::string CheckoutParts::Names() const
{
    std::string names;
    for (auto const& row: CheckoutPartTable)
    {
        if (!Contains(row.part))
            continue;
        if (!names.empty())
            names.push_back(',');
        names.append(row.name);
    }
    return names;
}

namespace
{
    /// @param bits Part bits, as the scan reports them. @return The same, as a set.
    [[nodiscard]] CheckoutParts PartsOfBits(std::uint8_t bits) noexcept
    {
        CheckoutParts parts;
        for (auto const part: Enumerators<CheckoutPart>())
            if ((bits & BitOf(part)) != 0)
                parts = parts.With(part);
        return parts;
    }

    /// @param parts A set. @return The same, as the scan's bits.
    [[nodiscard]] std::uint8_t BitsOfParts(CheckoutParts parts) noexcept
    {
        std::uint8_t bits = 0;
        for (auto const part: Enumerators<CheckoutPart>())
            if (parts.Contains(part))
                bits = static_cast<std::uint8_t>(bits | BitOf(part));
        return bits;
    }
} // namespace

std::vector<std::string> RootSpellings(CheckoutRoots const& checkout)
{
    std::vector<std::string> roots;
    std::ranges::transform(
        NeedlesOf(checkout), std::back_inserter(roots), [](Needle const& needle) { return needle.spelling; });
    return roots;
}

RootBindingScan ScanForRoots(std::span<std::byte const> image, std::span<std::string const> roots)
{
    // One part bit shared by every spelling, so the search stops at the first one found.
    std::vector<Needle> needles;
    std::ranges::transform(roots, std::back_inserter(needles), [](std::string const& root) {
        return Needle { .spelling = root, .folded = FoldRoot(root), .parts = 1 };
    });
    return ScanNeedles(image, needles, 1).first;
}

RootBindingScan ScanCheckout(std::span<std::byte const> image, CheckoutRoots const& checkout)
{
    auto [scan, bits] = ScanNeedles(image, NeedlesOf(checkout), BitsOfParts(CheckoutParts::All()));
    scan.parts = PartsOfBits(bits);
    if (scan.binding == RootBinding::Bound)
        scan.evidence += std::format("; bound to its {}", scan.parts.Names());
    return scan;
}

std::string ComputeRootBoundKey(std::string_view portableKey, CheckoutRoots const& checkout, CheckoutParts parts)
{
    // Its own schema tag, and the portable key folded WHOLE rather than its inputs: the
    // portable key already carries `objkey-v*`, so a bump there moves every bound key
    // with it and this tag only ever versions how the checkout is folded on top.
    //
    // The RESOLVED absolute value of each part the object names, and the part's NAME
    // before it, so a source root and a build tree that happen to be one directory still
    // key apart. `v2` because `v1` folded every part whatever the object named.
    KeyDigest digest { "rootbound-v2" };
    digest.Field(portableKey);
    for (auto const& row: CheckoutPartTable)
    {
        if (!parts.Contains(row.part))
            continue;
        digest.Field(row.name);
        digest.Path(row.resolved(checkout));
    }
    return digest.ToHex();
}

std::vector<std::byte> EncodeRootBoundMarker(CheckoutParts parts)
{
    auto const text = std::format("{}{} {}", MarkerPrefix, MarkerVersion, parts.Names());
    CompileValue marker;
    std::ranges::transform(text, std::back_inserter(marker.objectBlob), [](char c) { return static_cast<std::byte>(c); });
    return EncodeCompileValue(marker);
}

MarkerReading ReadRootBoundMarker(CompileValue const& value)
{
    std::string_view const blob { reinterpret_cast<char const*>(value.objectBlob.data()), value.objectBlob.size() };
    if (!value.textRegions.empty() || !blob.starts_with(MarkerPrefix))
        return { .kind = MarkerKind::Object, .parts = {} };

    auto const unreadable = MarkerReading { .kind = MarkerKind::UnreadableMarker, .parts = {} };
    auto rest = blob.substr(MarkerPrefix.size());
    if (!rest.starts_with(MarkerVersion) || rest.substr(MarkerVersion.size(), 1) != " ")
        return unreadable;
    rest.remove_prefix(MarkerVersion.size() + 1);

    CheckoutParts parts;
    for (auto const name: rest | std::views::split(',') | std::views::transform([](auto const& piece) {
                              return std::string_view { piece.begin(), piece.end() };
                          }))
    {
        auto const* const row = core::findIfOrNull(
            CheckoutPartTable, [name](CheckoutPartRow const& candidate) { return candidate.name == name; });
        if (row == nullptr)
            return unreadable;
        parts = parts.With(row->part);
    }
    // A marker naming no part would fold nothing and key every checkout alike.
    if (parts.Empty())
        return unreadable;
    return { .kind = MarkerKind::Marker, .parts = parts };
}

StorePlan PlanStore(std::string const& portableKey,
                    std::span<std::byte const> object,
                    CheckoutRoots const& checkout,
                    std::optional<CheckoutParts> metMarker)
{
    auto scan = ScanCheckout(object, checkout);
    if (metMarker.has_value() && scan.binding == RootBinding::Portable)
        scan = RootBindingScan { .binding = RootBinding::Bound,
                                 .evidence = std::format("found a root-bound marker at its portable key, which a "
                                                         "portable store would overwrite; bound to its {}",
                                                         metMarker->Names()),
                                 .parts = *metMarker };
    switch (scan.binding)
    {
        case RootBinding::Portable:
            return { .objectKey = portableKey, .markerKey = std::nullopt, .scan = std::move(scan) };
        case RootBinding::Bound:
            break;
    }
    auto objectKey = ComputeRootBoundKey(portableKey, checkout, scan.parts);
    return { .objectKey = std::move(objectKey), .markerKey = portableKey, .scan = std::move(scan) };
}

FetchedObject ResolveFetchedObject(std::span<std::byte const> payload,
                                   std::string const& portableKey,
                                   CheckoutRoots const& checkout,
                                   ValueFetch const& fetch)
{
    auto decoded = DecodeCompileValue(payload);
    if (!decoded.has_value())
        return { .answer = std::move(decoded.error()),
                 .servedKey = portableKey,
                 .followedMarker = false,
                 .markerParts = std::nullopt };
    auto const marker = ReadRootBoundMarker(*decoded);
    switch (marker.kind)
    {
        case MarkerKind::Object:
            return {
                .answer = std::move(*decoded), .servedKey = portableKey, .followedMarker = false, .markerParts = std::nullopt
            };
        case MarkerKind::UnreadableMarker:
            // Refused, never served and never guessed at: which parts to fold is exactly
            // what this build cannot read out of it.
            return { .answer = ProtocolError { .code = ProtocolErrorCode::MalformedFrame,
                                               .context = "a root-bound marker this build cannot read" },
                     .servedKey = portableKey,
                     .followedMarker = false,
                     .markerParts = std::nullopt };
        case MarkerKind::Marker:
            break;
    }

    auto boundKey = ComputeRootBoundKey(portableKey, checkout, marker.parts);
    auto const followed = [&boundKey, &marker](std::variant<CompileValue, BoundAbsence, ProtocolError> answer) {
        return FetchedObject { .answer = std::move(answer),
                               .servedKey = std::move(boundKey),
                               .followedMarker = true,
                               .markerParts = marker.parts };
    };
    auto const bound = fetch(boundKey);
    if (!bound.has_value())
        return followed(BoundAbsence {});

    auto boundValue = DecodeCompileValue(*bound);
    if (!boundValue.has_value())
        return followed(std::move(boundValue.error()));
    // Followed once and no further. Only a digest collision or a damaged store can put
    // a marker here, and chasing it would be a loop with no bound.
    if (ReadRootBoundMarker(*boundValue).kind != MarkerKind::Object)
        return followed(ProtocolError { .code = ProtocolErrorCode::MalformedFrame,
                                        .context = "a root-bound marker under a root-bound key" });
    return followed(std::move(*boundValue));
}

} // namespace FastCache::Cc
