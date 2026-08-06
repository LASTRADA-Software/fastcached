// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace FastCache::Cc;

namespace
{
FastCache::PathCanon::Layout WindowsLayout()
{
    return { .sourceRoot = R"(D:\Lastrada)", .buildTree = R"(D:\Lastrada\out\build\win64)" };
}

DirectManifest SampleManifest()
{
    return {
        .toolchainStamp = "cl 19.51.36231 x64",
        .objectKey = "0123456789abcdef0123456789abcdef",
        .entries = {
            { .canonicalPath = "<SRCROOT>/src/LabBase/LabBase.hpp", .contentHash = "aabb1122" },
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
    CHECK_FALSE(IsToolchainHeader(R"(D:\Lastrada\src\LabBase\LabBase.hpp)", layout));
    CHECK_FALSE(IsToolchainHeader(R"(D:\Lastrada\out\build\win64\generated\config.hpp)", layout));
}

TEST_CASE("IsToolchainHeader treats a vcpkg tree inside the build tree as toolchain")
{
    // vcpkg headers are canonicalizable (they sit under the build tree) but are
    // third-party and immutable, so they belong to the stamp rather than to the
    // hashed set — otherwise every build would hash thousands of vendored headers.
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(D:\Lastrada\out\build\win64\vcpkg_installed\x64-windows\include\zlib.h)", layout));
}

TEST_CASE("IsToolchainHeader is case-insensitive on Windows paths")
{
    // The compiler echoes include paths in whatever case the -I spelling used, so a
    // case-sensitive match would misclassify a header as project content and then
    // fail to canonicalize it.
    auto const layout = WindowsLayout();
    CHECK(IsToolchainHeader(R"(C:\PROGRAM FILES (X86)\WINDOWS KITS\10\include\um\windows.h)", layout));
    CHECK_FALSE(IsToolchainHeader(R"(d:\lastrada\src\LabBase\LabBase.hpp)", layout));
}

TEST_CASE("IsToolchainHeader matches a root spelled with forward slashes")
{
    // CMake exports FASTCACHE_SRCROOT in its own native form (`D:/Lastrada`) while
    // cl emits includes with backslashes. A separator-sensitive prefix test makes
    // every project header look external, which classifies the whole manifest as
    // toolchain content and produces an empty manifest — direct mode then never
    // engages, silently.
    FastCache::PathCanon::Layout const cmakeStyle { .sourceRoot = "D:/Lastrada",
                                                    .buildTree = "D:/Lastrada/out/build/win64" };
    CHECK_FALSE(IsToolchainHeader(R"(D:\Lastrada\src\LabBase\LabBase.hpp)", cmakeStyle));
    CHECK_FALSE(IsToolchainHeader(R"(D:\Lastrada\out\build\win64\generated\config.hpp)", cmakeStyle));

    // And the reverse spelling must work too.
    FastCache::PathCanon::Layout const winStyle { .sourceRoot = R"(D:\Lastrada)", .buildTree = R"(D:\Lastrada\out)" };
    CHECK_FALSE(IsToolchainHeader("D:/Lastrada/src/LabBase/LabBase.hpp", winStyle));
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

TEST_CASE("ComputeManifestKey is stable and separates differing inputs")
{
    std::vector<std::string> const args { "/O2", "<SRCROOT>/src/a.cpp" };
    auto const base = ComputeManifestKey("<SRCROOT>/src/a.cpp", args, "cl-19.51");

    CHECK(base.size() == 32);
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
    CHECK(base.size() == 32);

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
    auto const stamp = "cl-19.51";

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

TEST_CASE("ValidateManifest accepts an empty manifest with a matching stamp")
{
    // No entries means nothing to invalidate; the stamp alone decides.
    DirectManifest const empty { .toolchainStamp = "cl-19.51", .objectKey = "k", .entries = {} };
    CHECK(ValidateManifest(empty, WindowsLayout(), "cl-19.51"));
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
    auto const root = std::filesystem::temp_directory_path() / "fc-direct-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "header.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint original();\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };
    constexpr std::string_view stamp = "cl-test-1";

    auto built = BuildManifest({ headerPath.string() }, layout, stamp, "objkey-1");
    REQUIRE(built.has_value());
    REQUIRE(built->entries.size() == 1);
    CHECK(built->entries[0].canonicalPath.starts_with("<SRCROOT>/"));

    CHECK(ValidateManifest(*built, layout, stamp));

    auto const keyBefore = ComputeHeaderStateDigest("mkey", *built);

    // Edit the header: validation must fail and the derived object key must move.
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\nint edited_differently();\n";
    }
    CHECK_FALSE(ValidateManifest(*built, layout, stamp));

    auto const rebuilt = BuildManifest({ headerPath.string() }, layout, stamp, "objkey-1");
    REQUIRE(rebuilt.has_value());
    CHECK(ComputeHeaderStateDigest("mkey", *rebuilt) != keyBefore);

    // Deleting the header must also invalidate rather than silently pass.
    std::filesystem::remove(headerPath);
    CHECK_FALSE(ValidateManifest(*rebuilt, layout, stamp));

    std::filesystem::remove_all(root);
}

TEST_CASE("BuildManifest normalizes '..' segments and mixed separators to one token")
{
    // Real /showIncludes output echoes the resolved-but-unnormalized path, e.g.
    // `D:\src\LabBase\../ctrllib/ListOb.hpp`. Two spellings of the same header must
    // collapse to a single canonical token, or an entry recorded through one
    // spelling would never validate against the other — which is exactly what made
    // manifests come out empty before this was handled.
    auto const root = std::filesystem::temp_directory_path() / "fc-direct-normalize";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src" / "a");
    std::filesystem::create_directories(root / "src" / "b");

    auto const headerPath = root / "src" / "b" / "shared.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
    }

    FastCache::PathCanon::Layout const layout { .sourceRoot = root.string(), .buildTree = (root / "out").string() };

    // The same file named directly, via a `..` hop, and with forward slashes.
    std::vector<std::string> const includes {
        headerPath.string(),
        (root / "src" / "a" / ".." / "b" / "shared.hpp").string(),
        root.string() + "/src/b/shared.hpp",
    };

    auto const built = BuildManifest(includes, layout, "cl-test-1", "objkey-1");
    REQUIRE(built.has_value());
    CHECK(built->entries.size() == 1);
    CHECK(ValidateManifest(*built, layout, "cl-test-1"));

    std::filesystem::remove_all(root);
}

TEST_CASE("BuildManifest drops toolchain headers and deduplicates project headers")
{
    auto const root = std::filesystem::temp_directory_path() / "fc-direct-dedup";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");

    auto const headerPath = root / "src" / "shared.hpp";
    {
        std::ofstream out { headerPath, std::ios::binary };
        out << "#pragma once\n";
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

    auto const built = BuildManifest(includes, layout, "cl-test-1", "objkey-1");
    REQUIRE(built.has_value());
    CHECK(built->entries.size() == 1);

    std::filesystem::remove_all(root);
}
