// SPDX-License-Identifier: Apache-2.0
#include "ObjectSections.hpp"
#include "StubCoffTestSupport.hpp"
#include "StubElfMachOTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/Unwrap.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Test;

namespace
{

/// The names a walk reported, in order.
[[nodiscard]] std::vector<std::string> NamesOf(ObjectSections const& laidOut)
{
    std::vector<std::string> names;
    std::ranges::transform(
        laidOut.sections, std::back_inserter(names), [](ObjectSection const& section) { return section.name; });
    return names;
}

/// The bytes of the section called @p name, as text, or nothing when there is none.
[[nodiscard]] std::optional<std::string> DataOf(std::span<std::byte const> image,
                                                ObjectSections const& laidOut,
                                                std::string_view name)
{
    for (auto const& section: laidOut.sections)
        if (section.name == name)
        {
            auto const bytes = image.subspan(section.at, section.size);
            return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
        }
    return std::nullopt;
}

} // namespace

TEST_CASE("A COFF long section name is resolved through the string table", "[launcher][object]")
{
    // `.debug_info` is longer than the eight-byte name field, so a compiler spells it
    // `/4`. Left unresolved it could not be told from program data by any reader.
    auto const image =
        BuildCoff({ { .name = ".rdata", .data = "RDATA" }, { .name = ".debug_info", .data = "DWARF" } }, 1000);
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    auto const& walked = Testing::Unwrap(laidOut);
    CHECK(walked.format == ObjectFormat::Coff);
    CHECK(NamesOf(walked) == std::vector<std::string> { ".rdata", ".debug_info" });
    CHECK(DataOf(image, walked, ".debug_info") == "DWARF");
}

TEST_CASE("A /bigobj COFF object is walked by its own layout", "[launcher][object]")
{
    auto const image = BuildCoff(ClSections("CODE"), 1000, StubLayout::BigObj);
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    CHECK(NamesOf(Testing::Unwrap(laidOut)) == std::vector<std::string> { ".drectve", ".debug$S", ".text$mn", ".chks64" });
}

TEST_CASE("A COFF section claiming data past the end refuses the strict walk and not the lenient one", "[launcher][object]")
{
    auto image = BuildCoff({ { .name = ".rdata", .data = "RDATA" }, { .name = ".text", .data = "CODE" } }, 1000);
    // The second section's size field, made to run off the end of the file.
    PutLe(image, StubHeaderSize(StubLayout::Standard) + StubSectionHeaderSize + 16, 4, 0x7FFF'FFFF);
    CHECK_FALSE(ReadObjectSections(image).has_value());

    auto const* layout = ChooseCoffLayout(image);
    REQUIRE(layout != nullptr);
    auto const lenient = CoffSections(*layout, image);
    REQUIRE(lenient.size() == 1);
    CHECK(lenient.front().name == ".rdata");
}

TEST_CASE("An ELF object is walked in both classes and both byte orders", "[launcher][object]")
{
    std::vector<StubFormatSection> const sections { { .name = ".text", .data = "CODE" },
                                                    { .name = ".rodata.str1.1", .data = "STRINGS" },
                                                    { .name = ".bss", .data = "IGNORED", .type = 8 },
                                                    { .name = ".debug_str", .data = "DEBUG", .flags = 0 } };
    for (auto const is64: { true, false })
        for (auto const bigEndian: { false, true })
        {
            INFO("ELF" << (is64 ? 64 : 32) << (bigEndian ? " big-endian" : " little-endian"));
            auto const image = BuildElf(sections, { .is64 = is64, .bigEndian = bigEndian, .extendedNumbering = false });
            auto const laidOut = ReadObjectSections(image);
            REQUIRE(laidOut.has_value());
            auto const& walked = Testing::Unwrap(laidOut);
            CHECK(walked.format == ObjectFormat::Elf);
            // `.bss` holds no bytes in the file, so it is not listed.
            CHECK(NamesOf(walked) == std::vector<std::string> { ".text", ".rodata.str1.1", ".debug_str", ".shstrtab" });
            CHECK(DataOf(image, walked, ".rodata.str1.1") == "STRINGS");
        }
}

TEST_CASE("An ELF object past 65279 sections is walked through extended numbering", "[launcher][object]")
{
    // The real count lives in section 0 once a 16-bit field cannot hold it. A walk that
    // read `e_shnum` as zero sections would report nothing and call the object empty.
    auto const image = BuildElf({ { .name = ".rodata", .data = "STRINGS" } },
                                { .is64 = true, .bigEndian = false, .extendedNumbering = true });
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    CHECK(DataOf(image, Testing::Unwrap(laidOut), ".rodata") == "STRINGS");
}

TEST_CASE("An ELF compressed section is reported as compressed", "[launcher][object]")
{
    auto const image = BuildElf({ { .name = ".rodata", .data = "ZSTREAM", .flags = 0x2 | 0x800 } });
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    auto const& walked = Testing::Unwrap(laidOut);
    REQUIRE_FALSE(walked.sections.empty());
    CHECK(walked.sections.front().compressed);
}

TEST_CASE("A truncated ELF object is no layout at all", "[launcher][object]")
{
    auto image = BuildElf({ { .name = ".rodata", .data = "STRINGS" } });
    image.resize(image.size() - 8);
    CHECK_FALSE(ReadObjectSections(image).has_value());
}

TEST_CASE("A Mach-O object names each section by its segment", "[launcher][object]")
{
    auto const image = BuildMachO64({ { .name = "__cstring", .data = "STRINGS", .segment = "__TEXT" },
                                      { .name = "__debug_str", .data = "DEBUG", .segment = "__DWARF" } });
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    auto const& walked = Testing::Unwrap(laidOut);
    CHECK(walked.format == ObjectFormat::MachO);
    CHECK(NamesOf(walked) == std::vector<std::string> { "__TEXT,__cstring", "__DWARF,__debug_str" });
    CHECK(DataOf(image, walked, "__TEXT,__cstring") == "STRINGS");
}

TEST_CASE("A universal Mach-O is walked slice by slice", "[launcher][object]")
{
    // What `CMAKE_OSX_ARCHITECTURES=arm64;x86_64` hands a compile cache: one file, two
    // objects. A walk that read only the table would see no sections at all.
    auto const arm = BuildMachO64({ { .name = "__cstring", .data = "ARM", .segment = "__TEXT" } });
    auto const intel = BuildMachO64({ { .name = "__cstring", .data = "INTEL", .segment = "__TEXT" } });
    auto const image = BuildFatMachO({ arm, intel });
    auto const laidOut = ReadObjectSections(image);
    REQUIRE(laidOut.has_value());
    auto const& walked = Testing::Unwrap(laidOut);
    REQUIRE(walked.sections.size() == 2);
    auto const text = [&image](ObjectSection const& section) {
        auto const bytes = std::span<std::byte const> { image }.subspan(section.at, section.size);
        return std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() };
    };
    CHECK(text(walked.sections[0]) == "ARM");
    CHECK(text(walked.sections[1]) == "INTEL");
}

TEST_CASE("Bytes that are no object format are no layout", "[launcher][object]")
{
    std::string const text = "this is not an object file at all, just some text";
    std::vector<std::byte> image;
    std::ranges::transform(text, std::back_inserter(image), [](char c) { return static_cast<std::byte>(c); });
    CHECK_FALSE(ReadObjectSections(image).has_value());
    CHECK_FALSE(ReadObjectSections({}).has_value());
}
