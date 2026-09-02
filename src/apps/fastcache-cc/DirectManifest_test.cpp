// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"
#include "DirectManifest.hpp"
#include "KeyDigest.hpp"
#include "KeyDigestTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache::Cc;
using FastCache::Cc::Test::DigestQuarters;
using FastCache::Cc::Test::RequireNoRetiredGeneration;
using FastCache::Cc::Test::RetiredGeneration;
using FastCache::Cc::Test::SplitMix64;

namespace
{
FastCache::PathCanon::Layout WindowsLayout()
{
    return { .sourceRoot = R"(D:\Project)", .buildTree = R"(D:\Project\out\build\win64)" };
}

DirectManifest SampleManifest()
{
    return {
        .toolchainStamp = "cl 19.51.36231 x64",
        .objectKey = "0123456789abcdef0123456789abcdef",
        .entries = {
            { .canonicalPath = "<SRCROOT>/src/AppCore/AppCore.hpp", .contentHash = "aabb1122" },
            { .canonicalPath = "<SRCROOT>/src/Toolbox/unicode.hpp", .contentHash = "ccdd3344" },
        },
    };
}
} // namespace

TEST_CASE("EncodeManifest and DecodeManifest round-trip a manifest")
{
    auto const original = SampleManifest();
    auto const decoded = DecodeManifest(EncodeManifest(original));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == original);
}

TEST_CASE("DecodeManifest round-trips an empty entry list")
{
    DirectManifest const empty { .toolchainStamp = "cl 19.51", .objectKey = "k", .entries = {} };
    auto const decoded = DecodeManifest(EncodeManifest(empty));
    REQUIRE(decoded.has_value());
    CHECK(decoded->entries.empty());
    CHECK(decoded->toolchainStamp == "cl 19.51");
}

TEST_CASE("DecodeManifest rejects truncated input")
{
    auto encoded = EncodeManifest(SampleManifest());
    // Every prefix short of the whole is structurally incomplete; none may parse.
    for (std::size_t length = 0; length < encoded.size(); ++length)
    {
        auto const decoded = DecodeManifest(std::string_view { encoded }.substr(0, length));
        CHECK_FALSE(decoded.has_value());
    }
}

TEST_CASE("DecodeManifest rejects trailing garbage")
{
    auto encoded = EncodeManifest(SampleManifest());
    encoded.push_back('\x7f');
    auto const decoded = DecodeManifest(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == DirectError::Malformed);
}

TEST_CASE("DecodeManifest reports an unknown version distinctly")
{
    auto encoded = EncodeManifest(SampleManifest());
    encoded[0] = static_cast<char>(0xFE);
    auto const decoded = DecodeManifest(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == DirectError::UnknownVersion);
}

TEST_CASE("DecodeManifest rejects a length field that overruns the buffer")
{
    // Claim a 0xFFFFFF00-byte toolchain stamp in a handful of bytes. A decoder that
    // trusts the length would read far past the input.
    std::string hostile;
    hostile.push_back('\x01');
    hostile.append("\xff\xff\xff\x00", 4);
    hostile.append("short");
    auto const decoded = DecodeManifest(hostile);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == DirectError::Malformed);
}

namespace
{
/// A manifest blob declaring `count` entries and carrying `trailing` bytes after the
/// header for them to be decoded from.
[[nodiscard]] std::string BlobDeclaring(std::uint32_t count, std::size_t trailing)
{
    std::string out;
    out.push_back('\x01'); // version
    out.append(4, '\0');   // toolchainStamp: empty
    out.append(4, '\0');   // objectKey: empty
    for (auto const shift: { 24, 16, 8, 0 })
        out.push_back(static_cast<char>((count >> shift) & 0xFFU));
    out.append(trailing, '\0');
    return out;
}
} // namespace

TEST_CASE("DecodeManifest refuses an entry count the blob cannot supply")
{
    // Issue #267's class, third site. The case above proves a length field cannot
    // overrun the buffer; this proves a COUNT field cannot either, which is a
    // different question and was unguarded. This reserved straight from the `u32`
    // before reading a single entry, and an `Entry` is two `std::string`s -- sixty-four
    // bytes in memory against eight on the wire -- so the blob below asked for
    // roughly 274 GB.
    //
    // The bytes come off the network: the launcher fetches this manifest from the
    // cache server, so the count is a peer's number rather than its own.
    auto const hostile = BlobDeclaring(0xFFFFFFFFU, 0);
    REQUIRE(hostile.size() == 13);

    auto const decoded = DecodeManifest(hostile);
    // The refusal is the assertion. A decoder that merely survived would pass a
    // "did not crash" test and still be reserving.
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == DirectError::Malformed);
}

TEST_CASE("DecodeManifest bounds an entry count by the bytes actually left")
{
    // The boundary, so the guard is neither off by one nor a number somebody picked.
    // An entry needs eight bytes -- two empty length prefixes -- so eight trailing
    // bytes can carry exactly one.

    // Two entries cannot fit in eight bytes, and that is decided on the count alone.
    CHECK_FALSE(DecodeManifest(BlobDeclaring(2, 8)).has_value());

    // One entry does fit, and those eight zero bytes really are one entry: two empty
    // fields. So the guard admits exactly what the blob can carry rather than capping
    // it at something smaller.
    auto const exact = DecodeManifest(BlobDeclaring(1, 8));
    REQUIRE(exact.has_value());
    REQUIRE(exact->entries.size() == 1);
    CHECK(exact->entries[0].canonicalPath.empty());

    // And a manifest with no entries and nothing trailing is ordinary.
    CHECK(DecodeManifest(BlobDeclaring(0, 0)).has_value());
}

TEST_CASE("MinEntryBytes tracks the encoder rather than a comment")
{
    // The guard's per-element minimum is derived by hand from `EncodeManifest`'s loop,
    // which is comment discipline. This pins it structurally: the cost of one EMPTY
    // entry is measured from the encoder itself, so a field added to that loop fails
    // here rather than quietly leaving the guard weaker than the format it guards.
    DirectManifest empty { .toolchainStamp = {}, .objectKey = {}, .entries = {} };
    DirectManifest one = empty;
    one.entries.push_back(DirectManifest::Entry { .canonicalPath = {}, .contentHash = {} });

    CHECK(EncodeManifest(one).size() - EncodeManifest(empty).size() == 8);
}

TEST_CASE("A manifest encoded on one machine decodes identically on another")
{
    // The portability guarantee: the encoding holds canonical tokens only, so the
    // bytes carry no machine identity and a peer at a different checkout root
    // reconstructs exactly what the producer wrote. This is what lets direct-mode
    // manifests be shared, unlike sccache's local-disk-only equivalent.
    auto const produced = SampleManifest();
    auto const wire = EncodeManifest(produced);

    auto const consumed = DecodeManifest(wire);
    REQUIRE(consumed.has_value());
    CHECK(*consumed == produced);

    // Re-encoding on the consumer must reproduce the identical bytes, or the two
    // machines would compute different cache keys from the same build state.
    CHECK(EncodeManifest(*consumed) == wire);
}

TEST_CASE("IsToolchainHeader classifies the Windows SDK and MSVC as toolchain")
{
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(C:\Program Files (x86)\Windows Kits\10\include\10.0.26100.0\um\windows.h)", layout));
    CHECK(IsToolchainHeader(
        R"(C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\MSVC\14.51.36231\ATLMFC\include\afxwin.h)",
        layout));
}

TEST_CASE("IsToolchainHeader classifies project headers under the roots as project")
{
    auto const layout = WindowsLayout();
    CHECK_FALSE(IsToolchainHeader(R"(D:\Project\src\AppCore\AppCore.hpp)", layout));
    CHECK_FALSE(IsToolchainHeader(R"(D:\Project\out\build\win64\generated\config.hpp)", layout));
}

TEST_CASE("IsToolchainHeader treats a vcpkg tree inside the build tree as toolchain")
{
    // vcpkg headers are canonicalizable (they sit under the build tree) but are
    // third-party and immutable, so they belong to the stamp rather than to the
    // hashed set — otherwise every build would hash thousands of vendored headers.
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(D:\Project\out\build\win64\vcpkg_installed\x64-windows\include\zlib.h)", layout));
}

TEST_CASE("IsToolchainHeader is case-insensitive on Windows paths")
{
    // The compiler echoes include paths in whatever case the -I spelling used, so a
    // case-sensitive match would misclassify a header as project content and then
    // fail to canonicalize it.
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(C:\PROGRAM FILES (X86)\WINDOWS KITS\10\include\um\windows.h)", layout));
    CHECK_FALSE(IsToolchainHeader(R"(d:\project\src\AppCore\AppCore.hpp)", layout));
}

TEST_CASE("IsToolchainHeader matches a root spelled with forward slashes")
{
    // CMake exports FASTCACHE_SOURCE_DIR in its own native form (`D:/Project`) while
    // cl emits includes with backslashes. A separator-sensitive prefix test makes
    // every project header look external, which classifies the whole manifest as
    // toolchain content and produces an empty manifest — direct mode then never
    // engages, silently.
    FastCache::PathCanon::Layout const cmakeStyle { .sourceRoot = "D:/Project", .buildTree = "D:/Project/out/build/win64" };
    CHECK_FALSE(IsToolchainHeader(R"(D:\Project\src\AppCore\AppCore.hpp)", cmakeStyle));
    CHECK_FALSE(IsToolchainHeader(R"(D:\Project\out\build\win64\generated\config.hpp)", cmakeStyle));

    // And the reverse spelling must work too.
    FastCache::PathCanon::Layout const winStyle { .sourceRoot = R"(D:\Project)", .buildTree = R"(D:\Project\out)" };
    CHECK_FALSE(IsToolchainHeader("D:/Project/src/AppCore/AppCore.hpp", winStyle));
}

TEST_CASE("IsToolchainHeader treats a path under no known root as toolchain")
{
    // Such a path has no canonical form, so it cannot be listed per-file; riding the
    // stamp is the only representable choice.
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(E:\somewhere\else\foreign.hpp)", layout));
}

TEST_CASE("ParseIncludePaths extracts paths and ignores unrelated lines")
{
    constexpr std::string_view text = "Note: including file: C:\\sdk\\windows.h\r\n"
                                      "some_file.cpp\r\n"
                                      "Note: including file:   D:\\proj\\src\\a.hpp\r\n"
                                      "Note: including file:     D:\\proj\\src\\b.hpp\n";
    auto const paths = ParseIncludePaths(text);
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == R"(C:\sdk\windows.h)");
    // Nesting indentation and trailing CR must both be stripped.
    CHECK(paths[1] == R"(D:\proj\src\a.hpp)");
    CHECK(paths[2] == R"(D:\proj\src\b.hpp)");
}

TEST_CASE("ParseIncludePaths returns nothing for text with no include notes")
{
    CHECK(ParseIncludePaths("just a compiler warning\r\n").empty());
    CHECK(ParseIncludePaths("").empty());
}

TEST_CASE("ParseIncludePaths ignores a marker that is not the start of the line")
{
    // The recognition rule is shared with SplitIncludeNotes, which applies it to a
    // stream that also carries preprocessed source. Anchoring it after leading
    // blanks (cl indents by nesting depth) and nowhere else is what keeps an
    // ordinary line quoting the marker from being read as a dependency — and, on
    // the splitter's side, from being deleted out of the hashed text.
    CHECK(ParseIncludePaths("warning: Note: including file: C:\\x.h\r\n").empty());
    CHECK(ParseIncludePaths("char const* s = \"Note: including file: /usr/include/a.h\";\n").empty());
}

TEST_CASE("ComputeManifestKey is stable and separates differing inputs")
{
    std::vector<std::string> const args { "/O2", "<SRCROOT>/src/a.cpp" };
    auto const base = ComputeManifestKey("<SRCROOT>/src/a.cpp", args, "cl-19.51");

    CHECK(base.size() == KeyDigest::HexLength);
    CHECK(base == ComputeManifestKey("<SRCROOT>/src/a.cpp", args, "cl-19.51"));

    // Each input must participate: changing any one changes the key.
    CHECK(base != ComputeManifestKey("<SRCROOT>/src/b.cpp", args, "cl-19.51"));
    CHECK(base != ComputeManifestKey("<SRCROOT>/src/a.cpp", args, "cl-20.00"));
    CHECK(base != ComputeManifestKey("<SRCROOT>/src/a.cpp", { "/Od", "<SRCROOT>/src/a.cpp" }, "cl-19.51"));
}

TEST_CASE("ComputeHeaderStateDigest changes when any header hash changes")
{
    auto manifest = SampleManifest();
    auto const base = ComputeHeaderStateDigest("mkey", manifest);
    CHECK(base.size() == KeyDigest::HexLength);

    // The whole point of direct mode: an edited header must yield a different
    // object key, so the stale object is never served.
    manifest.entries[0].contentHash = "ffffffff";
    CHECK(ComputeHeaderStateDigest("mkey", manifest) != base);
}

TEST_CASE("ComputeHeaderStateDigest distinguishes manifest keys and entry sets")
{
    auto const manifest = SampleManifest();
    CHECK(ComputeHeaderStateDigest("keyA", manifest) != ComputeHeaderStateDigest("keyB", manifest));

    DirectManifest fewer = manifest;
    fewer.entries.pop_back();
    CHECK(ComputeHeaderStateDigest("keyA", fewer) != ComputeHeaderStateDigest("keyA", manifest));
}

TEST_CASE("Two machines with different checkout roots derive the same object key")
{
    // The cross-machine guarantee, end to end: the same logical build state on two
    // different checkout roots must produce one shared object key, or the two
    // machines would never hit each other's entries.
    std::vector<std::string> const args { "/O2", "<SRCROOT>/src/a.cpp" };
    auto const* const stamp = "cl-19.51";

    auto const keyHere = ComputeManifestKey("<SRCROOT>/src/a.cpp", args, stamp);
    auto const keyThere = ComputeManifestKey("<SRCROOT>/src/a.cpp", args, stamp);
    CHECK(keyHere == keyThere);

    // Manifests exchanged over the wire carry canonical tokens only, so a peer
    // decodes byte-identical content and derives the identical object key.
    auto const produced = SampleManifest();
    auto const roundTripped = DecodeManifest(EncodeManifest(produced));
    REQUIRE(roundTripped.has_value());
    CHECK(ComputeHeaderStateDigest(keyThere, *roundTripped) == ComputeHeaderStateDigest(keyHere, produced));
}

TEST_CASE("ValidateManifest rejects a manifest from a different toolchain")
{
    // Toolchain headers are not listed individually, so the stamp is the only thing
    // that can detect a compiler or SDK change.
    auto const layout = WindowsLayout();
    CHECK_FALSE(ValidateManifest(SampleManifest(), layout, "cl 20.00.00000 x64"));
}

TEST_CASE("ValidateManifest rejects entries whose files do not exist")
{
    // SampleManifest names headers under a checkout root that is not present here,
    // so localization succeeds but hashing fails — which must read as stale.
    auto const layout = WindowsLayout();
    CHECK_FALSE(ValidateManifest(SampleManifest(), layout, "cl 19.51.36231 x64"));
}

TEST_CASE("A compile whose every reported dependency was dropped records no manifest")
{
    // The guard that turns #319 from a wrong object into a miss.
    //
    // `IsToolchainHeader` reports every path outside both roots as toolchain, so a
    // header belonging to ANOTHER checkout classifies exactly as an SDK header does
    // and is dropped. When a hit replays a value whose regions were never
    // canonicalized, every path fed to BuildManifest is such a path -- so what would
    // be recorded is the translation unit and nothing else, and ValidateManifest
    // would then re-hash the TU, find it unchanged, and serve the object however the
    // headers move.
    //
    // Refused by name rather than recorded thin. The refusal costs direct mode for
    // this compile, exactly as `Unanchored` already does, and the ordinary
    // preprocessed key still serves it.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-hollow" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const sourcePath = root / "src" / "u.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "int main() { return 0; }\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = (root / "src").string(),
                                                .buildTree = (root / "bld").string() };

    // A path under neither root -- another checkout's header, which is exactly what
    // an uncanonicalized region replays.
    auto const refused = BuildManifest({ .sourcePath = sourcePath.string(),
                                         .includePaths = { R"(D:\some\other\checkout\src\dep.hpp)" },
                                         .workingDirectory = root.string(),
                                         .toolchainStamp = "cl-19.51",
                                         .objectKey = "objkey-1" },
                                       layout);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().fault == ManifestFault::NoProjectDeps);

    // A TU that reported nothing is NOT refused: it has no headers to drop, so the
    // source alone is the whole truth about it. The guard asks whether reported
    // dependencies were dropped, never how few entries came out -- a count test
    // would refuse this one too.
    auto const kept = BuildManifest({ .sourcePath = sourcePath.string(),
                                      .includePaths = {},
                                      .workingDirectory = root.string(),
                                      .toolchainStamp = "cl-19.51",
                                      .objectKey = "objkey-1" },
                                    layout);
    REQUIRE(kept.has_value());
    CHECK(kept->entries.size() == 1);
}

TEST_CASE("ValidateManifest refuses an empty manifest rather than validating on nothing")
{
    // This case used to assert the opposite -- "no entries means nothing to
    // invalidate; the stamp alone decides" -- and that reading is the vacuous truth
    // #319 turned into a wrong object. `all_of` over no entries is true, so such a
    // manifest does not pass a check, it skips one, and the object it points at is
    // served however the sources move.
    //
    // `BuildManifest` cannot produce one: the TU is always entry one and its
    // presence is that function's own precondition (issue #49 / issue #51). So an
    // empty entry set is a decode artifact or an older format, and a matching stamp
    // says nothing at all about the sources.
    DirectManifest const empty { .toolchainStamp = "cl-19.51", .objectKey = "k", .entries = {} };
    CHECK_FALSE(ValidateManifest(empty, WindowsLayout(), "cl-19.51"));
    CHECK_FALSE(ValidateManifest(empty, WindowsLayout(), "cl-20.00"));
}

TEST_CASE("HashFileContents returns empty for a missing file")
{
    CHECK(HashFileContents(R"(D:\definitely\not\here\nope.hpp)").empty());
}

TEST_CASE("Build then validate a manifest against real files on disk")
{
    // The end-to-end invalidation contract, exercised against the filesystem rather
    // than fixtures: a manifest validates while the headers are untouched, and stops
    // validating the moment one of them changes or disappears.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-test" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "header.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint original();\n";
    }

    auto const sourcePath = root / "src" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    constexpr std::string_view stamp = "cl-test-1";

    auto const inputs = [&](std::string_view objectKey) {
        return ManifestInputs { .sourcePath = sourcePath.string(),
                                .includePaths = { headerPath.string() },
                                .workingDirectory = root.string(),
                                .toolchainStamp = std::string { stamp },
                                .objectKey = std::string { objectKey } };
    };

    auto built = BuildManifest(inputs("objkey-1"), layout);
    REQUIRE(built.has_value());
    REQUIRE(built->entries.size() == 2); // the header, plus the TU BuildManifest always records
    CHECK(std::ranges::all_of(built->entries, [](auto const& e) { return e.canonicalPath.starts_with("<SRCROOT>/"); }));

    CHECK(ValidateManifest(*built, layout, stamp));

    auto const keyBefore = ComputeHeaderStateDigest("mkey", *built);

    // Edit the header: validation must fail and the derived object key must move.
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint edited_differently();\n";
    }
    CHECK_FALSE(ValidateManifest(*built, layout, stamp));

    auto const rebuilt = BuildManifest(inputs("objkey-1"), layout);
    REQUIRE(rebuilt.has_value());
    CHECK(ComputeHeaderStateDigest("mkey", *rebuilt) != keyBefore);

    // Deleting the header must also invalidate rather than silently pass.
    std::filesystem::remove(headerPath);
    CHECK_FALSE(ValidateManifest(*rebuilt, layout, stamp));

    std::filesystem::remove_all(root);
}

TEST_CASE("ValidateManifest catches an edit to the translation unit itself, MSVC-style")
{
    // /showIncludes (and therefore ParseIncludePaths) never names the primary
    // translation unit -- only the headers it pulls in. RecordManifest (main.cpp)
    // compensates by adding the TU's own source path to the list it hands to
    // BuildManifest, alongside the headers /showIncludes reported. Without that,
    // editing a .cpp's own body while leaving every header untouched is invisible
    // to ValidateManifest, and a direct-mode hit replays a stale object forever
    // (see issue #49 / issue #51).
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-source-edit" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "header.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint helper();\n";
    }

    auto const sourcePath = root / "src" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\nint main() { return 0; }\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    constexpr std::string_view stamp = "cl-test-1";

    // MSVC's /showIncludes lists only the header; the TU reaches BuildManifest as
    // its own `sourcePath` field, which is why a manifest can never omit it.
    auto built = BuildManifest({ .sourcePath = sourcePath.string(),
                                 .includePaths = { headerPath.string() },
                                 .workingDirectory = root.string(),
                                 .toolchainStamp = std::string { stamp },
                                 .objectKey = "objkey-1" },
                               layout);
    REQUIRE(built.has_value());
    REQUIRE(built->entries.size() == 2);
    CHECK(ValidateManifest(*built, layout, stamp));

    // Edit the .cpp body itself -- no header touched.
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\nint main() { return 1; }\n";
    }

    // The manifest must no longer validate: the TU itself is part of what a hit
    // reproduces, so its own content has to be covered too.
    CHECK_FALSE(ValidateManifest(*built, layout, stamp));

    std::filesystem::remove_all(root);
}

TEST_CASE("BuildManifest records a relative dependency path instead of dropping it")
{
    // The regression this case exists for. BuildManifest asked IsToolchainHeader
    // before classifying the anchor, and IsToolchainHeader reports EVERY path
    // outside both roots as toolchain -- which a relative path always is, since it
    // lies under no root at all. So a GNU build whose depfile carries relative
    // header paths (a relative `-I`, or a compile run from the source directory)
    // recorded a manifest of its absolute entries alone, silently. Edit one of the
    // dropped headers, leave the .cpp untouched, and the direct hit fires against a
    // manifest that never named the edited file: a stale object under a zero exit
    // code, which is the whole failure class direct mode's revalidation exists to
    // prevent. Cc::IsCheckable and Cc::PortableForm both order this correctly and
    // say so in comments; this was the third consumer and did not.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-relative" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "header.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint helper();\n";
    }

    auto const sourcePath = root / "src" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\nint main() { return 0; }\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    constexpr std::string_view stamp = "cc-test-1";

    // Both the source and the header spelled relatively, as `cc -c src/a.cpp` run
    // from the checkout root reports them. The working directory is passed in
    // rather than taken from the process, so this asserts the real resolution rule
    // instead of whatever directory the test binary happens to run in.
    auto const inputs = ManifestInputs { .sourcePath = "src/a.cpp",
                                         .includePaths = { "src/header.hpp" },
                                         .workingDirectory = root.string(),
                                         .toolchainStamp = std::string { stamp },
                                         .objectKey = "objkey-rel" };

    auto const built = BuildManifest(inputs, layout);
    REQUIRE(built.has_value());
    REQUIRE(built->entries.size() == 2);

    // Recorded as canonical tokens, not kept relative: a manifest entry has to be
    // localized back to a file on the validating machine, and a relative entry
    // could only be resolved against that machine's working directory.
    CHECK(built->entries[0].canonicalPath == "<SRCROOT>/src/a.cpp");
    CHECK(built->entries[1].canonicalPath == "<SRCROOT>/src/header.hpp");
    CHECK(ValidateManifest(*built, layout, stamp));

    // The property that matters: editing the relatively-named header must stop the
    // manifest validating. Before the fix it stayed valid forever, because the
    // header was never an entry.
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint helper(int);\n";
    }
    CHECK_FALSE(ValidateManifest(*built, layout, stamp));

    std::filesystem::remove_all(root);
}

TEST_CASE("BuildManifest records a relatively-named translation unit (issue #57)")
{
    // The same defect reaching the TU rather than a header, and the reason the
    // source is BuildManifest's own field: the CMake Ninja generator spells sources
    // relative to the build directory, so `cmd.source` arrived relative, was
    // classified as toolchain, and was dropped -- reopening issues #49/#51, whose
    // whole fix was to make the TU part of what a manifest revalidates.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-relative-tu" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "out");

    auto const headerPath = root / "src" / "header.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
    }

    auto const sourcePath = root / "src" / "t.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\nint main() { return 0; }\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    constexpr std::string_view stamp = "cc-test-1";

    // Run from the build tree, as Ninja does: `../src/t.cpp`.
    auto const inputs = ManifestInputs { .sourcePath = "../src/t.cpp",
                                         .includePaths = { headerPath.string() },
                                         .workingDirectory = (root / "out").string(),
                                         .toolchainStamp = std::string { stamp },
                                         .objectKey = "objkey-tu" };

    auto const built = BuildManifest(inputs, layout);
    REQUIRE(built.has_value());
    REQUIRE(built->entries.size() == 2);
    CHECK(built->entries[0].canonicalPath == "<SRCROOT>/src/header.hpp");
    CHECK(built->entries[1].canonicalPath == "<SRCROOT>/src/t.cpp");
    CHECK(ValidateManifest(*built, layout, stamp));

    // Edit the .cpp body only. Before the fix this validated forever.
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"header.hpp\"\nint main() { return 1; }\n";
    }
    CHECK_FALSE(ValidateManifest(*built, layout, stamp));

    std::filesystem::remove_all(root);
}

TEST_CASE("CanonicalSourceToken agrees whichever way the source is spelled")
{
    // One derivation for both sides. The lookup (TryDirectMode) and the recording
    // (RecordManifest) each build the manifest key from this token, so if they
    // disagreed about a relatively-named source they would key the same compile two
    // ways and direct mode could never hit. It is also what removes an ambiguity
    // that predates the relative-path bug: `cc -c ../src/t.cpp` run from `build/`
    // and from `build/sub/` relativizes to the SAME argument list, so a key derived
    // from the unresolved spelling names two different files.
    FastCache::PathCanon::Layout const layout { .sourceRoot = "/w/src", .buildTree = "/w/build" };

    auto const fromAbsolute = CanonicalSourceToken("/w/src/t.cpp", layout, "/w/build");
    auto const fromRelative = CanonicalSourceToken("../src/t.cpp", layout, "/w/build");
    REQUIRE(fromAbsolute.has_value());
    REQUIRE(fromRelative.has_value());
    CHECK(*fromAbsolute == "<SRCROOT>/t.cpp");
    CHECK(*fromRelative == *fromAbsolute);

    // Two depths that relativize identically must not collapse to one token.
    auto const fromDeeper = CanonicalSourceToken("../src/t.cpp", layout, "/w/build/sub");
    REQUIRE(fromDeeper.has_value());
    CHECK(*fromDeeper != *fromRelative);

    // A source under neither root has no portable form. Refusing costs the compile
    // direct mode; the ordinary preprocessed key still serves it.
    CHECK_FALSE(CanonicalSourceToken("/elsewhere/t.cpp", layout, "/w/build").has_value());
}

TEST_CASE("BuildManifest tells its refusals apart and names the path (issue #68)")
{
    // Refusals with nothing in common but the word "no". Each names a different
    // thing to go and fix -- a working directory, a layout, a root spelled almost
    // right, a file -- and the launcher has exactly one line in which to say which
    // happened. Collapsed into one value they printed "uncanonicalizable source or
    // include" for all of them alike, naming neither the cause nor the path, and a
    // translation unit that silently stopped shortcutting had no other trace at all.
    FastCache::PathCanon::Layout const layout { .sourceRoot = "/w/src", .buildTree = "/w/build" };

    auto const inputsWith = [&layout](std::string source, std::vector<std::string> includes, std::string cwd) {
        return BuildManifest({ .sourcePath = std::move(source),
                               .includePaths = std::move(includes),
                               .workingDirectory = std::move(cwd),
                               .toolchainStamp = "cc-test-1",
                               .objectKey = "objkey-1" },
                             layout);
    };

    SECTION("a source the working directory cannot place")
    {
        auto const built = inputsWith("src/t.cpp", {}, "");
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::Unanchored, .path = "src/t.cpp" });
        CHECK(DescribeManifestFailure(built.error()) == "unanchored: src/t.cpp");
    }

    SECTION("a source under neither root")
    {
        // An ordinary `add_subdirectory(../shared shared)` layout, not a broken
        // build -- so the note has to name the file rather than accuse the setup.
        auto const built = inputsWith("/elsewhere/t.cpp", {}, "/w/build");
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::OutsideRoots, .path = "/elsewhere/t.cpp" });
        CHECK(DescribeManifestFailure(built.error()) == "under no root: /elsewhere/t.cpp");
    }

    SECTION("a source that is rooted but looks vendored")
    {
        // IsToolchainHeader tests its markers BEFORE any root, deliberately, so a
        // TU inside `vcpkg_installed/` under the build tree classifies as toolchain
        // while being perfectly well rooted. Reporting that as "under no root"
        // would send the reader to fix roots that are already correct -- the exact
        // misdirection this vocabulary exists to remove.
        std::string const vendored = "/w/build/vcpkg_installed/x64-linux/src/t.cpp";
        auto const built = inputsWith(vendored, {}, "/w/build");
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::ToolchainLike, .path = vendored });
        CHECK(DescribeManifestFailure(built.error()) == std::format("matches a toolchain marker: {}", vendored));
    }

    SECTION("a source under a root spelled almost right")
    {
        // `/w/src-other` prefix-matches `/w/src` character-wise, so IsToolchainHeader
        // calls it project content, while Canonicalize's segment-wise test declines.
        // Distinct from OutsideRoots precisely because the remedy is: the root is
        // nearly correct, rather than the file being somewhere else entirely.
        auto const built = inputsWith("/w/src-other/t.cpp", {}, "/w/build");
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::Uncanonical, .path = "/w/src-other/t.cpp" });
        CHECK(DescribeManifestFailure(built.error()) == "no canonical form: /w/src-other/t.cpp");
    }
}

TEST_CASE("BuildManifest names the offending DEPENDENCY, not the source (issue #68)")
{
    // The source has to be real here: BuildManifest records the TU before it looks
    // at a single dependency, so a fictitious source would refuse first and every
    // case below would be testing the wrong path. Which is itself the property under
    // test -- the refusal must send the reader to the file that was actually wrong,
    // and reporting the source would send them to the one file that was fine.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-fault-dep" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const sourcePath = root / "src" / "t.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "int main() { return 0; }\n";
    }

    // Rooted at `<root>/src` rather than at `<root>`, so the near-miss path below
    // lies under neither root. Under a root of `<root>` it would canonicalize
    // perfectly and the section would be testing Unreadable by another name.
    FastCache::PathCanon::Layout const layout { .sourceRoot = (root / "src").string(),
                                                .buildTree = (root / "out").string() };
    auto const build = [&](std::vector<std::string> includes, std::string cwd) {
        return BuildManifest({ .sourcePath = sourcePath.string(),
                               .includePaths = std::move(includes),
                               .workingDirectory = std::move(cwd),
                               .toolchainStamp = "cc-test-1",
                               .objectKey = "objkey-1" },
                             layout);
    };

    SECTION("one the working directory cannot place")
    {
        // Refused, not dropped, and this is the case that says so: a path still
        // relative after resolution names a file this build cannot identify, so
        // recording a manifest without it would be the same silent stale serve by
        // another route -- whereas refusing merely costs this compile direct mode.
        // Reachable only when the working directory itself is unavailable, which is
        // why it is a refusal and not a fallback.
        //
        // Spelled through std::filesystem so the separator is the one this layout
        // uses -- these roots are the host's, and the reported path comes back in
        // the layout's own vocabulary.
        auto const relative = (std::filesystem::path { "sub" } / "header.hpp").string();
        auto const built = build({ relative }, "");
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::Unanchored, .path = relative });
    }

    SECTION("one under a root spelled almost right")
    {
        // `<root>/src-other` prefix-matches the source root character-wise, so
        // IsToolchainHeader calls it project content, while Canonicalize's
        // segment-wise test declines. Classified before it is opened, so it need not
        // exist -- which is what separates this from Unreadable below.
        auto const stray = (root / "src-other" / "generated.hpp").string();
        auto const built = build({ stray }, root.string());
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::Uncanonical, .path = stray });
    }

    SECTION("one that canonicalizes but cannot be read")
    {
        // The other cause the old single message covered. A path with a perfectly
        // good token that cannot be opened -- a generated header not yet written, a
        // permission, a race -- is a different problem from one with no canonical
        // form, and it used to arrive as `Malformed`: the same value DecodeManifest
        // returns for corrupt bytes off the wire.
        auto const missing = (root / "src" / "never-written.hpp").string();
        auto const built = build({ missing }, root.string());
        REQUIRE_FALSE(built.has_value());
        CHECK(built.error() == ManifestFailure { .fault = ManifestFault::Unreadable, .path = missing });
    }

    std::filesystem::remove_all(root);
}

TEST_CASE("CanonicalSourceToken's refusal carries the path and the reason (issue #68)")
{
    // The refusal a real build actually meets. RecordManifest asks here first and
    // returns before BuildManifest ever runs, so a source with no token used to cost
    // a translation unit direct mode permanently while printing nothing whatsoever
    // -- not even under FASTCACHE_VERBOSE.
    FastCache::PathCanon::Layout const layout { .sourceRoot = "/w/src", .buildTree = "/w/build" };

    auto const outsideRoots = CanonicalSourceToken("/elsewhere/t.cpp", layout, "/w/build");
    REQUIRE_FALSE(outsideRoots.has_value());
    CHECK(outsideRoots.error() == ManifestFailure { .fault = ManifestFault::OutsideRoots, .path = "/elsewhere/t.cpp" });

    auto const unanchored = CanonicalSourceToken("src/t.cpp", layout, "");
    REQUIRE_FALSE(unanchored.has_value());
    CHECK(unanchored.error() == ManifestFailure { .fault = ManifestFault::Unanchored, .path = "src/t.cpp" });

    auto const uncanonical = CanonicalSourceToken("/w/src-other/t.cpp", layout, "/w/build");
    REQUIRE_FALSE(uncanonical.has_value());
    CHECK(uncanonical.error() == ManifestFailure { .fault = ManifestFault::Uncanonical, .path = "/w/src-other/t.cpp" });
}

TEST_CASE("DescribeManifestFailure renders every fault it can be given")
{
    // Walks the table rather than naming four faults, so a fifth appended to
    // ManifestFault fails here unless it was given a word to be printed as -- the
    // failure mode a refusal that renders as `": /some/path"` would otherwise be.
    for (auto const& row: FaultTable)
    {
        CHECK_FALSE(row.label.empty());
        CHECK(DescribeManifestFailure({ .fault = row.fault, .path = "/w/src/t.cpp" })
              == std::format("{}: /w/src/t.cpp", row.label));
    }

    // `Last` is the table's length rather than a fault, so it indexes one past the
    // end. Nothing constructs a failure carrying it, which is precisely why an
    // unguarded read there would never be noticed.
    CHECK(DescribeManifestFailure({ .fault = ManifestFault::Last, .path = "/w/src/t.cpp" }) == "unknown: /w/src/t.cpp");
}

TEST_CASE("AnchorWorkingDirectory re-spells a symlinked cwd in the layout's vocabulary")
{
    // getcwd(3) answers with the kernel's RESOLVED path, while a layout's roots are
    // spelled however the build system was configured. Every root test here is a
    // string prefix comparison, so a build under a symlinked prefix (macOS `/tmp`,
    // any symlinked `/home` or `/mnt`) reports a working directory that shares no
    // prefix with the root it is actually inside -- and a relative path resolved
    // against it then canonicalizes to nothing, silently costing the build direct
    // mode. Found by the end-to-end test on macOS, whose `/var` is a symlink.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-anchor" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base / "real" / "sub");

    std::error_code ec;
    std::filesystem::create_directory_symlink(base / "real", base / "link", ec);
    if (ec)
    {
        // Symlink creation needs a privilege or developer mode on Windows. The rule
        // is unconditional; only this way of demonstrating it is not.
        std::filesystem::remove_all(base);
        SUCCEED("symlinks unavailable on this host");
        return;
    }

    // The layout names the root through the symlink; the cwd arrives resolved.
    FastCache::PathCanon::Layout const layout { .sourceRoot = (base / "link").string(),
                                                .buildTree = (base / "link" / "out").string() };
    auto const anchored = AnchorWorkingDirectory((base / "real" / "sub").string(), layout);
    CHECK(anchored == (base / "link" / "sub").string());

    // And the point of doing it: a relative path now resolves under the root.
    CHECK(CanonicalSourceToken("t.cpp", layout, anchored) == "<SRCROOT>/sub/t.cpp");

    std::filesystem::remove_all(base);
}

TEST_CASE("AnchorWorkingDirectory prefers the longest root and passes through the rest")
{
    // The same rule CanonicalizeOne applies: a build tree nested inside the source
    // root must anchor to the build tree, or the tail spliced back on would be
    // relative to the wrong one.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-anchor-longest" };
    auto const& base = scratch.Path();
    std::filesystem::create_directories(base / "proj" / "out" / "build");
    std::filesystem::create_directories(base / "elsewhere");

    FastCache::PathCanon::Layout const layout { .sourceRoot = (base / "proj").string(),
                                                .buildTree = (base / "proj" / "out").string() };

    CHECK(AnchorWorkingDirectory((base / "proj" / "out" / "build").string(), layout)
          == (base / "proj" / "out" / "build").string());

    // Under neither root: handed back normalized, and the caller then finds it
    // anchors nothing -- which is a refusal, not a silent drop.
    auto const outside = AnchorWorkingDirectory((base / "elsewhere").string(), layout);
    CHECK_FALSE(CanonicalSourceToken("t.cpp", layout, outside).has_value());

    std::filesystem::remove_all(base);
}

TEST_CASE("ResolveAgainst anchors by the layout's conventions, not the host's")
{
    // std::filesystem::path::is_absolute() reads `D:\src\a.hpp` as relative on every
    // host but Windows, and would then glue a working directory in front of it --
    // producing a path under no root, which is the classification this whole change
    // is about getting right. PathCanon::AnchorForLayout is the single definition,
    // and it answers from the layout.
    FastCache::PathCanon::Layout const windows { .sourceRoot = R"(D:\Project)", .buildTree = R"(D:\Project\out)" };
    FastCache::PathCanon::Layout const posix { .sourceRoot = "/w/src", .buildTree = "/w/build" };

    // The correction itself, both ways round, so neither host can pass by accident.
    // std::filesystem answers with the HOST's preferred separator, so on a Windows
    // host `/w/src/a.hpp` comes back backslash-separated and AnchorForLayout --
    // which for a POSIX layout asks only about a leading `/` -- then reads it as
    // relative. DependencyProbe's PortableForm had the only copy of this rule and
    // the manifest side did not; Windows CI is what said so.
    CHECK(NormalizeForLayout("/w/src/a.hpp", posix) == "/w/src/a.hpp");
    CHECK(NormalizeForLayout(R"(D:\Project\src\a.hpp)", windows) == R"(D:\Project\src\a.hpp)");
    // A mixed spelling, which fails on EITHER host without the correction -- so this
    // one assertion covers the rule wherever the suite happens to run.
    CHECK(NormalizeForLayout(R"(/w/src\a.hpp)", posix) == "/w/src/a.hpp");
    // The correction runs one way only, deliberately, exactly as PortableForm's copy
    // did: it forces the POSIX layout's `/` and leaves a Windows layout alone.
    //
    // So for a Windows layout there is no output SPELLING to assert -- and that is
    // the substance rather than a gap in the test. `make_preferred()` answers `\` on
    // a Windows host and leaves `/` alone on a POSIX one, so pinning either string
    // pins a host; an earlier revision of this case pinned the POSIX one and CI on
    // Windows rejected it. Nothing downstream needs a spelling here: PathCanon calls
    // `C:/src/proj` a Windows layout too, and every prefix test unifies separators
    // before comparing. What must hold is the PROPERTY, and it holds either way.
    //
    // Only the POSIX direction can mislead, which is why only it is forced: there a
    // backslash is an ordinary filename character rather than a separator spelled
    // differently, so `AnchorForLayout` reads a backslash-separated path as relative
    // instead of as the absolute path it is.
    CHECK(FastCache::PathCanon::AnchorForLayout(NormalizeForLayout("D:/Project/src/a.hpp", windows), windows)
          == FastCache::PathCanon::Anchor::Absolute);

    CHECK(ResolveAgainst(R"(D:\Project\src\a.hpp)", R"(D:\Project\out)", windows) == R"(D:\Project\src\a.hpp)");
    CHECK(ResolveAgainst("/w/src/a.hpp", "/w/build", posix) == "/w/src/a.hpp");
    CHECK(ResolveAgainst("../src/a.hpp", "/w/build", posix) == "/w/src/a.hpp");
    // Normalized on both sides of the join, so one header cannot become two entries.
    CHECK(ResolveAgainst("./sub/../a.hpp", "/w/build", posix) == "/w/build/a.hpp");
    // No working directory means no anchoring; the caller decides what that means.
    CHECK(ResolveAgainst("src/a.hpp", "", posix) == "src/a.hpp");
}

TEST_CASE("BuildManifest normalizes '..' segments and mixed separators to one token")
{
    // Real /showIncludes output echoes the resolved-but-unnormalized path, e.g.
    // `D:\src\AppCore\../applib/Widget.hpp`. Two spellings of the same header must
    // collapse to a single canonical token, or an entry recorded through one
    // spelling would never validate against the other — which is exactly what made
    // manifests come out empty before this was handled.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-normalize" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src" / "a");
    std::filesystem::create_directories(root / "src" / "b");

    auto const headerPath = root / "src" / "b" / "shared.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
    }

    auto const sourcePath = root / "src" / "a" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"../b/shared.hpp\"\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };

    // The same file named directly, via a `..` hop, with forward slashes, and once
    // more relative to the working directory -- four spellings, one entry.
    std::vector<std::string> const includes {
        headerPath.string(),
        (root / "src" / "a" / ".." / "b" / "shared.hpp").string(),
        root.string() + "/src/b/shared.hpp",
        "src/a/../b/shared.hpp",
    };

    auto const built = BuildManifest({ .sourcePath = sourcePath.string(),
                                       .includePaths = includes,
                                       .workingDirectory = root.string(),
                                       .toolchainStamp = "cl-test-1",
                                       .objectKey = "objkey-1" },
                                     layout);
    REQUIRE(built.has_value());
    // The header once, plus the TU.
    CHECK(built->entries.size() == 2);
    CHECK(std::ranges::count(
              built->entries, std::string { "<SRCROOT>/src/b/shared.hpp" }, &DirectManifest::Entry::canonicalPath)
          == 1);
    CHECK(ValidateManifest(*built, layout, "cl-test-1"));

    std::filesystem::remove_all(root);
}

TEST_CASE("BuildManifest drops toolchain headers and deduplicates project headers")
{
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-dedup" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "shared.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
    }

    auto const sourcePath = root / "src" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"shared.hpp\"\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };

    // The same project header included three times, plus toolchain headers that must
    // not appear at all (they ride the stamp).
    std::vector<std::string> const includes {
        headerPath.string(),
        R"(C:\Program Files (x86)\Windows Kits\10\include\um\windows.h)",
        headerPath.string(),
        R"(C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Tools\MSVC\14.51\include\vector)",
        headerPath.string(),
    };

    auto const built = BuildManifest({ .sourcePath = sourcePath.string(),
                                       .includePaths = includes,
                                       .workingDirectory = root.string(),
                                       .toolchainStamp = "cl-test-1",
                                       .objectKey = "objkey-1" },
                                     layout);
    REQUIRE(built.has_value());
    // The project header once, plus the TU; neither toolchain header appears.
    CHECK(built->entries.size() == 2);

    std::filesystem::remove_all(root);
}

// --- GNU depfile parsing ----------------------------------------------------

TEST_CASE("ParseDepFileTargets names the outputs and skips the phony rules")
{
    // The two halves of one grammar, and the split is what the launcher needs: a
    // rule's target is an output the BUILD SYSTEM named, its dependencies are what
    // the compiler reported, and only the second may have its spelling reconciled.
    constexpr std::string_view depFile = "build/a.o: src/a.cpp src/inc/h1.h\n"
                                         "\n"
                                         "src/inc/h1.h:\n";

    auto const targets = ParseDepFileTargets(depFile);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0] == "build/a.o");

    // The phony rule -MP emits per header names a path the COMPILER reported, so
    // it is NOT an output: leaving it unreconciled would send a consumer a depfile
    // pointing -MP's deleted-header protection at files it cannot stat.
    CHECK(std::ranges::find(targets, "src/inc/h1.h") == targets.end());

    // And the dependency side is unchanged by the split.
    auto const deps = ParseDepFilePaths(depFile);
    REQUIRE(deps.size() == 2);
    CHECK(deps[0] == "src/a.cpp");
}

TEST_CASE("ParseDepFileTargets reads what the file says, not what the command line did")
{
    // Structural on purpose. Comparing against a parsed `-MT` value fails twice
    // over: gcc CONCATENATES repeated `-MT`, so a rule can carry several targets
    // while the parser keeps one; and `-MQ` escapes make-special characters on the
    // way out, so the token in the file need not equal any argument at all.
    constexpr std::string_view several = "build/a.o build/a.d: src/a.cpp\n";
    auto const many = ParseDepFileTargets(several);
    REQUIRE(many.size() == 2);
    CHECK(many[0] == "build/a.o");
    CHECK(many[1] == "build/a.d");

    // `-MQ 'b$uild/a.o'` reaches the file as `b$$uild/a.o`; the token is whatever
    // the file holds, which is exactly what the rewriter will compare against.
    constexpr std::string_view escaped = "b$$uild/a.o: src/a.cpp\n";
    auto const dollar = ParseDepFileTargets(escaped);
    REQUIRE(dollar.size() == 1);
    CHECK(dollar[0] == "b$$uild/a.o");
}

TEST_CASE("ParseDepFileTargets keeps a Windows drive letter out of the rule separator")
{
    // The same drive rule the dependency side uses, because it is one walker: a
    // `C:` that is a drive prefix is part of the target, not the end of it.
    constexpr std::string_view depFile = R"(D:\proj\build\a.obj: D:\proj\src\a.cpp)"
                                         "\n";
    auto const targets = ParseDepFileTargets(depFile);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0] == R"(D:\proj\build\a.obj)");
}

TEST_CASE("ParseDepFileTargets reaches a target on a continued line")
{
    // gcc wraps at ~76 columns, so a rule's two halves are routinely on different
    // physical lines; the splice happens before the rule is split, for both sides.
    constexpr std::string_view depFile = "build/a.o: \\\n"
                                         "  src/a.cpp \\\n"
                                         "  src/inc/h1.h\n";
    auto const targets = ParseDepFileTargets(depFile);
    REQUIRE(targets.size() == 1);
    CHECK(targets[0] == "build/a.o");
    CHECK(ParseDepFilePaths(depFile).size() == 2);
}

TEST_CASE("ParseDepFilePaths reads the dependencies of a simple rule")
{
    // The target (before the colon) is an OUTPUT, not a dependency: listing it
    // would put the object file into the manifest and make every validation
    // compare an object against a recorded header hash.
    auto const paths = ParseDepFilePaths("build/a.o: src/a.cpp src/a.hpp /usr/include/stdio.h\n");
    REQUIRE(paths.size() == 3);
    CHECK(paths[0] == "src/a.cpp");
    CHECK(paths[1] == "src/a.hpp");
    CHECK(paths[2] == "/usr/include/stdio.h");
}

TEST_CASE("ParseDepFilePaths follows backslash-newline continuations")
{
    // gcc wraps at ~76 columns, so a parser that stopped at the first newline
    // would see only the first few headers of a real translation unit — a
    // manifest that silently under-records its dependencies is worse than none.
    constexpr std::string_view depFile = "build/a.o: src/a.cpp \\\n"
                                         "  src/one.hpp \\\n"
                                         "  src/two.hpp \\\n"
                                         "  src/three.hpp\n";
    auto const paths = ParseDepFilePaths(depFile);
    REQUIRE(paths.size() == 4);
    CHECK(paths[1] == "src/one.hpp");
    CHECK(paths[3] == "src/three.hpp");
}

TEST_CASE("ParseDepFilePaths handles CRLF continuations")
{
    auto const paths = ParseDepFilePaths("a.o: a.cpp \\\r\n  b.hpp\r\n");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "a.cpp");
    CHECK(paths[1] == "b.hpp");
}

TEST_CASE("ParseDepFilePaths unescapes spaces inside a path")
{
    // A checkout under "C:\My Projects\..." or "~/Code Reviews/..." is ordinary,
    // and make escapes the space; splitting on it would yield two bogus paths.
    auto const paths = ParseDepFilePaths(R"(a.o: src/My\ File.hpp other.hpp)"
                                         "\n");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "src/My File.hpp");
    CHECK(paths[1] == "other.hpp");
}

TEST_CASE("ParseDepFilePaths keeps a Windows drive letter intact")
{
    // "C:" must not read as a rule separator, or every absolute Windows path
    // would be truncated to its drive and the manifest would be nonsense.
    auto const paths = ParseDepFilePaths(R"(D:\b\a.obj: D:\s\a.cpp D:\s\a.hpp)"
                                         "\n");
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == R"(D:\s\a.cpp)");
    CHECK(paths[1] == R"(D:\s\a.hpp)");
}

TEST_CASE("ParseDepFilePaths ignores the phony targets -MP emits")
{
    // -MP appends a bare `header:` rule per dependency so a deleted header does
    // not break the build. Those lines declare no dependencies of their own.
    constexpr std::string_view depFile = "a.o: a.cpp a.hpp\n"
                                         "\n"
                                         "a.cpp:\n"
                                         "\n"
                                         "a.hpp:\n";
    auto const paths = ParseDepFilePaths(depFile);
    REQUIRE(paths.size() == 2);
    CHECK(paths[0] == "a.cpp");
    CHECK(paths[1] == "a.hpp");
}

TEST_CASE("ParseDepFilePaths returns nothing for text that is not a rule")
{
    CHECK(ParseDepFilePaths("").empty());
    CHECK(ParseDepFilePaths("no colon here\n").empty());
    CHECK(ParseDepFilePaths("a.o:\n").empty()); // a rule with no dependencies
}

TEST_CASE("A GNU depfile drives a manifest exactly as showIncludes notes do")
{
    // The point of parsing depfiles at all: on POSIX the GNU drivers report
    // dependencies ONLY here, so without this direct mode can never populate —
    // it would pay for a manifest lookup on every compile and never hit.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-depfile" };
    auto const& root = scratch.Path();
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "dep.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
    }

    auto const sourcePath = root / "src" / "a.cpp";
    {
        std::ofstream out { sourcePath, std::ios::binary };
        out << "#include \"dep.hpp\"\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };

    // A depfile as gcc -MD -MF writes it: the object as the target, then the
    // source and its headers, wrapped across a continuation.
    auto const depFile =
        std::format("{}: {} \\\n  {}\n", (root / "out" / "a.o").string(), sourcePath.string(), headerPath.string());

    auto const paths = ParseDepFilePaths(depFile);
    REQUIRE(paths.size() == 2);

    // The depfile names the source among its own dependencies, so this also covers
    // the source field deduplicating against the include list.
    auto const built = BuildManifest({ .sourcePath = sourcePath.string(),
                                       .includePaths = paths,
                                       .workingDirectory = root.string(),
                                       .toolchainStamp = "gcc-test-1",
                                       .objectKey = "objkey-dep" },
                                     layout);
    REQUIRE(built.has_value());
    CHECK(built->entries.size() == 2); // the source and its header, both under the root
    CHECK(ValidateManifest(*built, layout, "gcc-test-1"));

    // Editing the recorded header must invalidate the manifest — otherwise a
    // direct hit would serve an object built from stale headers.
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint changed();\n";
    }
    CHECK_FALSE(ValidateManifest(*built, layout, "gcc-test-1"));

    std::filesystem::remove_all(root);
}

// --- issue #63: the manifest key must be as wide as it looks -----------------
//
// The mirror of the guard in CacheKey_test.cpp, and deliberately a separate CASE
// rather than one case covering both: these were two independently copy-pasted
// salted-CRC constructions (this file's `Lane`/`WideDigest` and CacheKey.cpp's
// `Lane`), and the point of consolidating them onto one digest is that a future
// divergence is caught on both sides rather than on whichever one a test
// happened to reach. Direct mode is on by default, so this key space is the one
// most builds actually use. The generator and the quarter split are shared
// (`KeyDigestTestSupport.hpp`) precisely because they must NOT diverge — two
// copies of a "deterministic" sequence are two sequences the moment one is
// touched, and the sample counts here are chosen against a measured collision
// index.

TEST_CASE("The quarters of a manifest key vary independently for equal-length inputs")
{
    // Under the salted-CRC construction each of the six pairwise XORs took
    // exactly one value here. EQUAL LENGTH IS THE POINT: the broken construction
    // varies freely once the lengths differ, so varying-width sources would leave
    // a test that passes against the bug it exists to catch.
    constexpr std::size_t Samples = 64;

    SplitMix64 source { 63 };
    std::vector<std::string> const args { "/O2" };
    std::array<std::set<std::uint32_t>, 6> pairwiseXors;
    for ([[maybe_unused]] auto const sample: std::views::iota(std::size_t { 0 }, Samples))
    {
        auto const quarters = DigestQuarters(ComputeManifestKey(source.NextFixedWidthText(), args, "cl-19.51"));

        std::size_t pair = 0;
        for (auto const i: std::views::iota(std::size_t { 0 }, quarters.size()))
            for (auto const j: std::views::iota(i + 1, quarters.size()))
                pairwiseXors[pair++].insert(quarters[i] ^ quarters[j]);
    }

    for (auto const index: std::views::iota(std::size_t { 0 }, pairwiseXors.size()))
    {
        INFO("quarter pair index " << index);
        CHECK(pairwiseXors[index].size() == Samples);
    }
}

TEST_CASE("The three launcher key spaces are separated by their schema tag")
{
    // With the salts gone, the leading schema tag is the whole of the domain
    // separation — and it is a stronger guarantee than the salts were. A tag is
    // NUL-free and NUL-terminated, so tag+NUL is a prefix-free code: "objkey-v3",
    // "manifest-v3" and "header-state-v1" differ in their first byte, which makes
    // the hashed blobs unequal by construction. A salt only made a cross-domain
    // collision improbable; disjoint inputs make it an ordinary collision, now at
    // 2^-128 rather than 2^-32.
    //
    // The inputs below are arranged so that everything after the tag is as alike
    // as the three signatures allow, which is the case a shared digest would fail
    // if the tags were ever dropped. All THREE are compared: the object key is the
    // space the other two exist to point at, so leaving it out would name the
    // property in the title and assert two thirds of it.
    std::vector<std::string> const args {};
    DirectManifest const manifest;

    // objkey folds compilerId then preprocessed as two `Field`s, exactly as the
    // manifest key folds toolchainStamp then canonicalSource -- so with the tags
    // stripped these two blobs would be byte-identical.
    auto const objectKey =
        ComputeKey(KeyInputs { .compilerId = "same", .preprocessed = "same", .relativizedArgs = {}, .dependencyPaths = {} });
    auto const manifestKey = ComputeManifestKey("same", args, "same");
    auto const headerState = ComputeHeaderStateDigest("same", manifest);

    CHECK(objectKey != manifestKey);
    CHECK(objectKey != headerState);
    CHECK(manifestKey != headerState);
}

TEST_CASE("The manifest digests are pinned, so changing them is deliberate")
{
    // See the note on the matching case in CacheKey_test.cpp for what to do when
    // this fails. The manifest key is half of a lock-step pair, and the coupling is
    // ONE-WAY: it must be bumped whenever `objkey-v*` is, because a manifest stores
    // the object key by value and its own key never sees the object-key schema. The
    // reverse is free -- v4 moved this tag alone, to retire the manifests recorded
    // while every relative dependency path was silently dropped, which no change to
    // the object key was needed for. v5 is the same direction again (issue #111),
    // retiring the manifests recorded before a drive-relative path under no root
    // was refused; `objkey` moved with it to keep the two tags from sitting apart,
    // not because anything forced it.
    // Computed ONCE and asserted twice, for the reason the matching case in
    // CacheKey_test.cpp records: the rows below are digests of these exact inputs,
    // so spelling the call a second time is a place for the two assertions to come
    // to disagree about what was hashed.
    std::vector<std::string> const args { "/O2", "<SRCROOT>/src/a.cpp" };
    auto const manifestKey = ComputeManifestKey("<SRCROOT>/src/a.cpp", args, "cl-19.51");
    CHECK(manifestKey == "2f8e995262b738bea0d31e062b6d4682");

    // And the generations it has retired stay retired. This is the half the vector
    // above cannot cover: a v5 manifest is only safe because no launcher carrying
    // the refusal ever records a defective one, so putting `manifest-v4` back makes
    // every pre-#104 manifest reachable again and direct mode serves a stale object
    // under a zero exit code. See RetiredGeneration.
    constexpr auto Retired = std::to_array<RetiredGeneration>({
        { .tag = "manifest-v3", .digest = "76b19c2b7caf3e0db4dcc1efcecb76aa" },
        { .tag = "manifest-v4", .digest = "8221eaeac6f3f8e52e523507780ed186" },
        { .tag = "manifest-v5", .digest = "72ed897f5c3dd9ec9fc2b4607aafdc96" },
    });
    RequireNoRetiredGeneration(manifestKey, Retired);

    // The header-state digest is deliberately NOT part of that pair: nothing is
    // stored under it, so it has no stale entries to re-key and its tag stays at
    // v1 while the other two move. Pinned all the same, because it is still a
    // value two runs have to agree on.
    DirectManifest manifest;
    manifest.toolchainStamp = "cl 19.51.36231 x64";
    manifest.objectKey = "0123456789abcdef0123456789abcdef";
    manifest.entries.push_back({ .canonicalPath = "<SRCROOT>/inc/a.hpp", .contentHash = "aaaa" });
    manifest.entries.push_back({ .canonicalPath = "<SRCROOT>/inc/b.hpp", .contentHash = "bbbb" });

    CHECK(ComputeHeaderStateDigest("mkey", manifest) == "6937b8627813a98102e756fa21856149");
}

TEST_CASE("HashFileContents separates equal-length contents and reports unreadable files")
{
    // The direct-mode revalidation hash. It used to be one CRC32C paired with the
    // byte count, which left 32 bits against exactly the case that matters: a
    // header edit preserving length. A collision there does not miss -- it decides
    // an edited header is unchanged and serves the stale object under a zero exit
    // code (issue #63, same defect as the key itself).
    // Cleared before it is populated, like every other filesystem case in this
    // file. The teardown at the end does not cover a run that crashed or was
    // interrupted, and this case asserts that `absent.hpp` is ABSENT -- so a
    // leftover of that name would make it fail for a reason that has nothing to
    // do with hashing.
    FastCache::Testing::ScratchDirectory const scratch { "fc-direct-hashfile" };
    auto const& dir = scratch.Path();
    std::filesystem::create_directories(dir);

    auto const write = [&](std::string_view name, std::string_view contents) {
        auto const path = dir / name;
        std::ofstream out { path, std::ios::binary };
        out << contents;
        out.close();
        return path.string();
    };

    auto const a = write("a.hpp", "static int value = 1;\n");
    auto const b = write("b.hpp", "static int value = 2;\n");
    auto const empty = write("empty.hpp", "");

    CHECK(HashFileContents(a).size() == KeyDigest::HexLength);
    CHECK(HashFileContents(a) == HashFileContents(a));
    CHECK(HashFileContents(a) != HashFileContents(b));

    // An unreadable file reports the empty string, and that must stay distinct
    // from every real digest: ValidateManifest compares this value for equality,
    // so an unreadable header must not compare equal to anything -- including a
    // readable one that happens to be empty.
    CHECK(HashFileContents((dir / "absent.hpp").string()).empty());
    CHECK_FALSE(HashFileContents(empty).empty());

    std::filesystem::remove_all(dir);
}

TEST_CASE("NormalizePath answers for a spelling this host cannot read, instead of throwing")
{
    // A launcher's one promise is that it never fails a build the compiler would
    // have completed, and `std::filesystem::path`'s narrow constructor THROWS on a
    // host that decodes narrow bytes as UTF-8 when the bytes are not -- which is
    // every Windows executable in this tree since it began declaring the UTF-8 code
    // page. `/showIncludes` under a legacy console produces exactly such bytes.
    //
    // Where that case is DECIDED is `RootReconciler::Path`, which reads a tool's
    // path as text first and counts what it could not, so main.cpp can decline the
    // compile. This is the guard that keeps the road there from ending in
    // std::terminate -- including for a path out of a manifest an older launcher
    // stored, which no reconciler ever sees.
    CHECK_NOTHROW(NormalizePath("/x/src/gr\xFC"
                                "n/a.h"));
    CHECK(NormalizePath("/x/src/gr\xFC"
                        "n/a.h")
          == "/x/src/gr\xFC"
             "n/a.h");

    // Still normalizes everything it can read, which is every path a build has.
    CHECK(NormalizePath("/x/src/./inc/../a.h") == std::filesystem::path { "/x/src/a.h" }.make_preferred().string());
}

namespace
{
/// Two checkouts of one project, differing exactly as the two in issue #368 did:
/// the translation unit is BYTE-IDENTICAL and a header it depends on is not.
///
/// That combination is the whole point. A manifest naming the TU and no header
/// revalidates on the TU hash alone, so it is sound precisely when the TU is the
/// only thing that matters -- and catastrophic when a header moved underneath it.
struct TwoCheckouts
{
    FastCache::Testing::ScratchDirectory root { "manifest-two-checkouts" };

    TwoCheckouts()
    {
        Make("checkout-old", "constexpr int Answer = 1;\n");
        Make("checkout-new", "constexpr int Answer = 2;\n");
    }

    /// @param name   Checkout directory name.
    /// @param header Contents of the header, which is what differs between them.
    void Make(std::string_view name, std::string_view header) const
    {
        auto const src = root.Path() / name / "src";
        auto error = std::error_code {};
        std::filesystem::create_directories(src, error);
        // Byte-identical in both checkouts, deliberately.
        Write(src / "tu.cpp", "#include \"dep.hpp\"\nint Value() { return Answer; }\n");
        Write(src / "dep.hpp", header);
    }

    static void Write(std::filesystem::path const& at, std::string_view text)
    {
        std::ofstream out { at, std::ios::binary };
        out << text;
    }

    [[nodiscard]] FastCache::PathCanon::Layout LayoutOf(std::string_view name) const
    {
        auto const base = root.Path() / name;
        return { .sourceRoot = base.string(), .buildTree = (base / "out").string() };
    }

    [[nodiscard]] std::string Source(std::string_view name) const
    {
        return (root.Path() / name / "src" / "tu.cpp").string();
    }

    [[nodiscard]] std::string Header(std::string_view name) const
    {
        return (root.Path() / name / "src" / "dep.hpp").string();
    }
};

constexpr std::string_view Stamp = "cl 19.51.36231 x64";
} // namespace

TEST_CASE("A manifest naming the TU and no header revalidates in a checkout it was not built from")
{
    // Issue #368's mechanism, shown rather than described. This is the state the
    // installed launcher reached, and the assertion below is what it cost.
    //
    // How it was reached there: the node stored values without canonicalizing
    // their text regions, so a replayed dependency record named the PRODUCING
    // checkout's headers. Every one of those paths lies outside this checkout's
    // roots, `IsToolchainHeader` calls every such path toolchain, all of them
    // drop -- and what is recorded is the TU and nothing else. That route is
    // closed twice over now (`CanonicalStoredValue` on the node, `NoProjectDeps`
    // here -- and "A compile whose every reported dependency was dropped records
    // no manifest" above already pins that refusal), which is why this case
    // reaches the same state the only way still open: by reporting NO
    // dependencies at all. `NoProjectDeps` is `!includePaths.empty() && recorded
    // == 0`, so an empty reported set skips it, and `ManifestAssertsNothing` asks
    // only whether the manifest is empty. A TU-only manifest is therefore still
    // constructible, and this is what it does.
    TwoCheckouts const checkouts;

    auto const hollow = BuildManifest({ .sourcePath = checkouts.Source("checkout-old"),
                                        .includePaths = {},
                                        .workingDirectory = checkouts.LayoutOf("checkout-old").sourceRoot,
                                        .toolchainStamp = std::string { Stamp },
                                        .objectKey = "object-from-the-old-checkout" },
                                      checkouts.LayoutOf("checkout-old"));
    REQUIRE(hollow.has_value());
    // The TU and nothing else. `entries.size()` rather than a header count,
    // because the TU is always entry one.
    REQUIRE(hollow->entries.size() == 1);

    // And here is the hazard: it validates in the OTHER checkout, whose header
    // differs, and vouches for an object built against the header it does not
    // mention. The manifest key is a function of the canonical source token and
    // the relativized args, so both checkouts address this manifest by
    // construction -- that portability is the feature, and this is its cost when
    // the entry set is hollow.
    CHECK(ValidateManifest(*hollow, checkouts.LayoutOf("checkout-new"), Stamp));

    // `ManifestAssertsNothing` does NOT catch it: it asks whether the manifest is
    // EMPTY, and this one names the TU. Asserted so the guard's edge is recorded
    // rather than assumed -- a reader who believes it covers this case would stop
    // looking exactly where the remaining exposure is.
    CHECK_FALSE(ManifestAssertsNothing(*hollow));
}

TEST_CASE("An honest manifest still records its header, and still fails in the other checkout")
{
    // The guard above must not be the reason everything refuses. A compile whose
    // dependency lies under its own roots records it, validates at home, and --
    // the property the hollow manifest lost -- FAILS where that header differs.
    TwoCheckouts const checkouts;

    auto const sound = BuildManifest({ .sourcePath = checkouts.Source("checkout-old"),
                                       .includePaths = { checkouts.Header("checkout-old") },
                                       .workingDirectory = checkouts.LayoutOf("checkout-old").sourceRoot,
                                       .toolchainStamp = std::string { Stamp },
                                       .objectKey = "object-key" },
                                     checkouts.LayoutOf("checkout-old"));
    REQUIRE(sound.has_value());
    CHECK(sound->entries.size() == 2);

    CHECK(ValidateManifest(*sound, checkouts.LayoutOf("checkout-old"), Stamp));
    // The header differs there, so this is the answer the hollow manifest could
    // not give.
    CHECK_FALSE(ValidateManifest(*sound, checkouts.LayoutOf("checkout-new"), Stamp));
}
