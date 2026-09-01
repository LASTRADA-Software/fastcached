// SPDX-License-Identifier: Apache-2.0
#include "ObjectEquivalence.hpp"

#include "IProcessRunner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// One section of a stub object.
struct StubSection
{
    std::string name;
    std::string data;
};

/// Write @p value into @p bytes at @p at, little-endian, over @p width bytes.
/// @param bytes The image being built.
/// @param at Where the field starts.
/// @param width How wide it is.
/// @param value What to store.
void PutLe(std::vector<std::byte>& bytes, std::size_t at, std::size_t width, std::uint64_t value)
{
    for (std::size_t index = 0; index < width; ++index)
        bytes[at + index] = static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
}

/// Which of the two COFF header layouts a stub is built to.
enum class StubLayout : std::uint8_t
{
    Standard,
    BigObj,
};

/// Build a structurally valid COFF object.
///
/// Synthetic rather than compiled, because most of what has to be asserted here
/// cannot be produced on demand by a compiler: an object whose `.text$mn` differs
/// while everything else matches is what a WRONG object looks like, and no
/// arrangement of flags makes `cl` emit one. The real-compiler cases below cover the
/// other half -- that the shape assumed here is the shape a compiler actually emits.
///
/// @param sections The sections, in file order.
/// @param timestamp What to put in `TimeDateStamp`.
/// @param layout Which header layout to write.
/// @param version The `/bigobj` version field; ignored for the standard layout.
/// @return The image.
[[nodiscard]] std::vector<std::byte> BuildCoff(std::vector<StubSection> const& sections,
                                               std::uint32_t timestamp,
                                               StubLayout layout = StubLayout::Standard,
                                               std::uint16_t version = 2)
{
    constexpr std::size_t SectionHeader = 40;
    constexpr std::size_t Symbols = 2;
    auto const big = layout == StubLayout::BigObj;
    auto const headerSize = big ? std::size_t { 56 } : std::size_t { 20 };
    auto const symbolRecord = big ? std::size_t { 20 } : std::size_t { 18 };

    auto const tableAt = headerSize;
    auto const dataAt = tableAt + (SectionHeader * sections.size());

    std::size_t payload = 0;
    for (auto const& section: sections)
        payload += section.data.size();

    auto const symbolsAt = dataAt + payload;
    auto const stringsAt = symbolsAt + (symbolRecord * Symbols);
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
        PutLe(image, 52, 4, Symbols);
    }
    else
    {
        PutLe(image, 0, 2, 0x8664);
        PutLe(image, 2, 2, sections.size());
        PutLe(image, 4, 4, timestamp);
        PutLe(image, 8, 4, symbolsAt);
        PutLe(image, 12, 4, Symbols);
        PutLe(image, 16, 2, 0);
        PutLe(image, 18, 2, 0);
    }

    auto cursor = dataAt;
    for (std::size_t index = 0; index < sections.size(); ++index)
    {
        auto const at = tableAt + (SectionHeader * index);
        auto const& section = sections[index];
        for (std::size_t byte = 0; byte < 8 && byte < section.name.size(); ++byte)
            image[at + byte] = static_cast<std::byte>(section.name[byte]);
        PutLe(image, at + 16, 4, section.data.size());
        PutLe(image, at + 20, 4, section.data.empty() ? 0 : cursor);
        for (std::size_t byte = 0; byte < section.data.size(); ++byte)
            image[cursor + byte] = static_cast<std::byte>(section.data[byte]);
        cursor += section.data.size();
    }

    PutLe(image, stringsAt, 4, 4);
    return image;
}

/// The sections a `cl` object carries, near enough for these cases.
/// @param code What `.text$mn` holds.
/// @param objectPath What the driver's path record holds.
/// @param sourceHash What the source checksum record holds.
/// @return The sections, in the order `cl` emits them.
[[nodiscard]] std::vector<StubSection> ClSections(std::string code,
                                                  std::string objectPath = "C:\\build\\tu.obj",
                                                  std::string sourceHash = "01234567")
{
    return { { .name = ".drectve", .data = "-defaultlib:libcpmt" },
             { .name = ".debug$S", .data = std::move(objectPath) },
             { .name = ".text$mn", .data = std::move(code) },
             { .name = ".chks64", .data = std::move(sourceHash) } };
}

/// Locate an MSVC-family driver, if this host has one on PATH.
///
/// The translation unit compiled through it includes NO header, which is what makes
/// this runnable without a developer command prompt: a bare `cl.exe` with no
/// `INCLUDE` still compiles it, and still emits the `.debug$S` and `.chks64` records
/// that make MSVC's output interesting here (measured).
/// @return The driver name, or nothing when none answers.
[[nodiscard]] std::optional<std::string> FindMsvcDriver()
{
    auto const runner = MakeProcessRunner();
    for (auto const* driver: { "cl.exe", "clang-cl.exe" })
    {
        // `cl` has no `--version` and answers its banner to a bare invocation; every
        // other MSVC-family driver answers one too. Either way a driver that is not
        // there cannot be spawned, and that is the whole question.
        std::vector<std::string> const argv { driver };
        if (runner->RunCaptureCombined(argv).exitCode != NotSpawned)
            return std::string { driver };
    }
    return std::nullopt;
}

/// Read a whole file.
/// @param path What to read.
/// @return Its bytes; empty when it could not be read.
[[nodiscard]] std::vector<std::byte> ReadAll(std::filesystem::path const& path)
{
    std::ifstream in { path, std::ios::binary };
    std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    std::vector<std::byte> bytes;
    bytes.reserve(raw.size());
    for (auto const c: raw)
        bytes.push_back(static_cast<std::byte>(c));
    return bytes;
}

} // namespace

TEST_CASE("Two identical objects are identical", "[launcher][verify][object]")
{
    auto const image = BuildCoff(ClSections("CODE"), 1000);
    auto const result = CompareObjectImages(image, image);
    CHECK(result.outcome == ObjectComparison::Identical);
    CHECK(result.detail.empty());
}

TEST_CASE("A COFF object differing only in its clock is the same compile", "[launcher][verify][object]")
{
    // THE regression test for #493, and it is the whole defect: a cached object was
    // produced before the fresh one it is checked against, by construction, so a
    // `memcmp` answered "wrong object" for every hit on Windows. Measured on MSVC
    // 14.51 and clang-cl, two compiles of one translation unit to one object path
    // differ here and nowhere else.
    auto const served = BuildCoff(ClSections("CODE"), 1000);
    auto const fresh = BuildCoff(ClSections("CODE"), 2000);
    REQUIRE(served.size() == fresh.size());

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::EquivalentApartFromVolatile);
    CHECK(result.detail.contains("timestamp"));
}

TEST_CASE("Different code is still caught after the clock is normalised", "[launcher][verify][object]")
{
    // The acceptance clause that matters more than the fix. A verifier that stops
    // crying wolf by no longer looking would pass the case above perfectly, so the
    // guard is shown to BITE -- with the clock differing too, which is the only
    // arrangement a real hit ever presents.
    auto const served = BuildCoff(ClSections("CODE"), 1000);
    auto const corrupted = BuildCoff(ClSections("C0DE"), 2000);
    REQUIRE(served.size() == corrupted.size());

    auto const result = CompareObjectImages(served, corrupted);
    CHECK(result.outcome == ObjectComparison::Different);
    CHECK(result.detail.contains(".text$mn"));
}

TEST_CASE("One byte of code, deep in the object, is caught", "[launcher][verify][object]")
{
    // An object file that differs in one instruction still links, so a comparison
    // that checked a prefix or a length would pass exactly the object worth catching.
    auto code = std::string(8192, 'x');
    auto const served = BuildCoff(ClSections(code), 1000);
    code[5000] = 'y';
    auto const corrupted = BuildCoff(ClSections(code), 1000);

    CHECK(CompareObjectImages(served, corrupted).outcome == ObjectComparison::Different);
}

TEST_CASE("An object built in another checkout is reported, not excused", "[launcher][verify][object]")
{
    // #489: two checkouts on one machine, one key, objects differing only in the
    // absolute path `cl` records in `.debug$S`. Excusing that section would cure
    // #493's false positives by no longer looking -- and would go silent on the
    // case an operator turns verification on to catch.
    //
    // It is reported as a DIFFERENCE, and the message says which kind, because a
    // foreign build path and stale code are acted on differently and "wrong object"
    // alone names neither.
    auto const served = BuildCoff(ClSections("CODE", "C:\\checkout-a\\tu.obj"), 1000);
    auto const fresh = BuildCoff(ClSections("CODE", "C:\\checkout-b\\tu.obj"), 2000);
    REQUIRE(served.size() == fresh.size());

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::Different);
    CHECK(result.detail.contains(".debug$S"));
    CHECK(result.detail.contains("different path"));
    CHECK_FALSE(result.detail.contains(".text$mn"));
}

TEST_CASE("A differing source checksum is reported as a place, not as code", "[launcher][verify][object]")
{
    auto const served = BuildCoff(ClSections("CODE", "C:\\build\\tu.obj", "AAAAAAAA"), 1000);
    auto const fresh = BuildCoff(ClSections("CODE", "C:\\build\\tu.obj", "BBBBBBBB"), 1000);

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::Different);
    CHECK(result.detail.contains(".chks64"));
}

TEST_CASE("A path record AND different code reads as different code", "[launcher][verify][object]")
{
    // The classification must not be reachable by adding a path difference to a real
    // one: an object that differs in `.text$mn` is a wrong object whatever else it
    // also differs in.
    auto const served = BuildCoff(ClSections("CODE", "C:\\checkout-a\\tu.obj"), 1000);
    auto const fresh = BuildCoff(ClSections("C0DE", "C:\\checkout-b\\tu.obj"), 1000);

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::Different);
    CHECK(result.detail.contains(".text$mn"));
    CHECK_FALSE(result.detail.contains("different path"));
}

TEST_CASE("A shorter object is a difference and says so", "[launcher][verify][object]")
{
    auto const served = BuildCoff(ClSections("CODE"), 1000);
    auto const truncated = std::span<std::byte const> { served }.first(served.size() - 8);

    auto const result = CompareObjectImages(truncated, served);
    CHECK(result.outcome == ObjectComparison::Different);
    CHECK(result.detail.contains("bytes"));
}

TEST_CASE("The clock's offset comes from the layout, not from a constant", "[launcher][verify][object]")
{
    // `/bigobj` is an allowed argument, so such a build is cacheable and reaches the
    // verifier. Its header is a different structure entirely and its clock is at
    // offset 8 -- a comparison that excused offset 4 would excuse the VERSION field
    // there while still reporting every hit as wrong.
    auto const served = BuildCoff(ClSections("CODE"), 1000, StubLayout::BigObj);
    auto const fresh = BuildCoff(ClSections("CODE"), 2000, StubLayout::BigObj);
    CHECK(CompareObjectImages(served, fresh).outcome == ObjectComparison::EquivalentApartFromVolatile);

    // And offset 4 there is NOT excused: it is the version, and two objects differing
    // in it are not the same compile.
    auto other = served;
    other[4] = static_cast<std::byte>(3);
    CHECK(CompareObjectImages(served, other).outcome == ObjectComparison::Different);
}

TEST_CASE("A standard object's offset 8 is not excused either", "[launcher][verify][object]")
{
    // The converse of the case above, and the reason the table is read rather than
    // both offsets being tolerated: offset 8 in a standard header is the symbol table
    // pointer, and two objects disagreeing about where their symbols are differ.
    auto const served = BuildCoff(ClSections("CODE"), 1000);
    auto other = served;
    other[8] = static_cast<std::byte>(static_cast<unsigned char>(other[8]) ^ 0x10U);
    CHECK(CompareObjectImages(served, other).outcome == ObjectComparison::Different);
}

TEST_CASE("An MSVC object format this build cannot read refuses by name", "[launcher][verify][object]")
{
    // Four states, and this is the one that must never read as `Different`. A
    // verifier that cannot tell "the cache is wrong" from "objects carry a clock this
    // build cannot find" has to SAY so: reported as a mismatch it is a precise wrong
    // answer, which is worse than none and is how the instrument gets switched off.
    auto const served = BuildCoff(ClSections("CODE"), 1000, StubLayout::BigObj, /*version=*/1);
    auto const fresh = BuildCoff(ClSections("CODE"), 2000, StubLayout::BigObj, /*version=*/1);

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::Unsupported);
    CHECK_FALSE(result.detail.empty());
}

TEST_CASE("An unreadable SERVED object is a finding, not a refusal", "[launcher][verify][object]")
{
    // Which of the two images decides is load-bearing and asymmetric. `fresh` is this
    // toolchain's own output, so a format this cannot lay out is this code's blind
    // spot; `served` came off the wire, and one that will not parse when the fresh
    // one does is a truncated transfer -- a real finding, and one of the few faults
    // distribution can actually introduce.
    auto const fresh = BuildCoff(ClSections("CODE"), 2000);
    std::vector<std::byte> served(fresh.size(), std::byte { 0xAB });

    auto const result = CompareObjectImages(served, fresh);
    CHECK(result.outcome == ObjectComparison::Different);
}

TEST_CASE("Nothing is excused in a format that has no clock", "[launcher][verify][object]")
{
    // Measured: clang and GCC objects are byte-identical between two compiles of one
    // translation unit, with and without `-g`, and across source directories. So ELF
    // keeps the strict byte comparison it always had -- normalising a field there
    // would overlook a real difference to fix a problem that platform does not have.
    std::vector<std::byte> served(512, std::byte { 0 });
    std::ranges::copy(std::array { std::byte { 0x7F }, std::byte { 'E' }, std::byte { 'L' }, std::byte { 'F' } },
                      served.begin());
    auto fresh = served;
    fresh[5] = std::byte { 0x99 };

    CHECK(CompareObjectImages(served, fresh).outcome == ObjectComparison::Different);
}

TEST_CASE("A real compiler's two objects verify clean, and a corrupted one does not",
          "[launcher][verify][object][msvc]")
{
    // The synthetic cases above assert the RULE; this one asserts that the rule
    // describes what a compiler emits. Running the verifier on made-up bytes would
    // prove the parser self-consistent and nothing about MSVC -- which is the whole
    // way #493 survived review, as a byte comparison that was never run on Windows.
    auto const driver = FindMsvcDriver();
    if (!driver.has_value())
        SKIP("no MSVC-family driver on PATH");

    Testing::ScratchDirectory const scratch { "object-equivalence-msvc" };
    auto const& dir = scratch.Path();
    auto const source = dir / "tu.cpp";
    {
        // No `#include`, deliberately: a bare driver with no `INCLUDE` compiles this,
        // so the case needs no developer command prompt, and `cl` still writes the
        // `.debug$S` and `.chks64` records that make its output worth testing.
        std::ofstream out { source };
        out << "namespace probe { int Scale(int x) { return (x * 3) + 1; } }\n"
            << "int Entry(int x) { return probe::Scale(x) - 2; }\n";
    }

    auto const object = dir / "tu.obj";
    auto const runner = MakeProcessRunner();
    std::vector<std::string> const argv {
        *driver, "/nologo", "/c", "/Fo" + object.string(), source.string()
    };

    REQUIRE(runner->RunCaptureCombined(argv).exitCode == 0);
    auto const first = ReadAll(object);
    REQUIRE_FALSE(first.empty());

    // The same command line again, to the same path -- which is exactly what the
    // verifier does, and why the driver's path records cannot differ.
    REQUIRE(runner->RunCaptureCombined(argv).exitCode == 0);
    auto const second = ReadAll(object);
    REQUIRE_FALSE(second.empty());

    auto const clean = CompareObjectImages(first, second);
    INFO("verdict detail: " << clean.detail);
    CHECK((clean.outcome == ObjectComparison::Identical
           || clean.outcome == ObjectComparison::EquivalentApartFromVolatile));

    // And the guard bites on the same real object. One byte in the back half, which
    // is code or data on every layout these drivers emit.
    auto corrupted = second;
    auto const at = corrupted.size() - (corrupted.size() / 4);
    corrupted[at] = static_cast<std::byte>(static_cast<unsigned char>(corrupted[at]) ^ 0xFFU);
    CHECK(CompareObjectImages(first, corrupted).outcome == ObjectComparison::Different);
}
