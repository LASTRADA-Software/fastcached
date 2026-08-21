// SPDX-License-Identifier: Apache-2.0
#include "DependencyProbe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{
/// The POSIX layout the filter cases are judged against.
PathCanon::Layout PosixLayout()
{
    return { .sourceRoot = "/home/dev/proj", .buildTree = "/home/dev/proj/build" };
}

std::vector<std::string> KeySet(std::vector<std::string> const& raw, PathCanon::Layout const& layout)
{
    return KeyDependencySet(std::span<std::string const> { raw }, layout);
}
} // namespace

TEST_CASE("KeyDependencySet keeps the paths under either root as canonical tokens")
{
    // The point of the whole exercise: these are the project's own headers, the
    // ones that move, and their token form is what makes the key differ after a
    // move while staying identical across two checkouts at different roots.
    auto const out = KeySet({ "/home/dev/proj/inc/new/Hdr.hpp", "/home/dev/proj/build/gen/Config.hpp" }, PosixLayout());

    REQUIRE(out.size() == 2);
    CHECK(out[0] == "<BUILDTREE>/gen/Config.hpp");
    CHECK(out[1] == "<SRCROOT>/inc/new/Hdr.hpp");
}

TEST_CASE("KeyDependencySet drops an absolute path under neither root")
{
    // Load-bearing exclusion. A toolchain header's absolute path is this machine's
    // alone, and the compiler identity in the key already covers the toolchain as a
    // whole. Hashing it would mean two machines with the same compiler at different
    // install prefixes share NOTHING — a duplicate entry set for every TU, to fix a
    // defect that only ever affects the depfile.
    auto const out = KeySet({ "/usr/include/c++/16/vector", "/opt/gcc-16/include/vector" }, PosixLayout());

    CHECK(out.empty());
}

TEST_CASE("KeyDependencySet keeps a relative path")
{
    // A relative path resolves against the compile's working directory, so it names
    // the same file on any machine running the same build. It lies under no root
    // either, so it must be classified BEFORE the absolute test or it would be
    // dropped along with the toolchain headers.
    auto const out = KeySet({ "inc/Local.hpp", "../shared/Util.hpp" }, PosixLayout());

    REQUIRE(out.size() == 2);
    CHECK(out[0] == "../shared/Util.hpp");
    CHECK(out[1] == "inc/Local.hpp");
}

TEST_CASE("KeyDependencySet normalizes a relative path's separators")
{
    // Canonical tokens already arrive with forward slashes, so without this the
    // same file reached through two spellings would key as two dependencies.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(inc\a.hpp)", "inc/a.hpp" }, windows);

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "inc/a.hpp");
}

TEST_CASE("KeyDependencySet sorts and deduplicates")
{
    // /showIncludes repeats a header once per inclusion site — hundreds of notes
    // for a few dozen files — and sorting makes the key insensitive to an emission
    // order that is a property of the driver rather than of what was compiled.
    auto const first = KeySet({ "/home/dev/proj/b.hpp", "/home/dev/proj/a.hpp", "/home/dev/proj/b.hpp" }, PosixLayout());
    auto const second = KeySet({ "/home/dev/proj/a.hpp", "/home/dev/proj/b.hpp" }, PosixLayout());

    REQUIRE(first.size() == 2);
    CHECK(first[0] == "<SRCROOT>/a.hpp");
    CHECK(first[1] == "<SRCROOT>/b.hpp");
    CHECK(first == second);
}

TEST_CASE("KeyDependencySet classifies a Windows path by the layout, not by the host")
{
    // A cache is shared across machines, so `C:\...` must read as absolute even
    // when this binary runs on POSIX. std::filesystem::path::is_absolute() answers
    // from the host and would send every Windows path down the relative branch,
    // where a toolchain path would then be kept and baked into the key.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(C:\src\proj\inc\a.hpp)", R"(C:\Program Files\MSVC\include\vector)" }, windows);

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet drops a drive-relative path but keeps a relative one")
{
    // The two must not collapse into one branch. `src\rel.hpp` resolves against
    // the compile's working directory and names the same file on every machine
    // running the same build, so it is keyed. `C:foo\bar.hpp` resolves against
    // drive C's own current directory — per-process state on the producing
    // machine that no cache entry records — so keying it would let two machines
    // whose C: cwd differs produce the SAME key for DIFFERENT headers. Before
    // issue #65 the classifier reported it as absolute, which dropped it too, but
    // by way of an answer that was not true.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(C:foo\bar.hpp)", R"(src\rel.hpp)", "C:" }, windows);

    CHECK(out == std::vector<std::string> { "src/rel.hpp" });
}

TEST_CASE("A drive-relative path under a drive-relative root is still keyed")
{
    // The anchor alone must not decide. `C:src\proj` is a Windows root (backslash
    // separators) that is itself drive-relative, so every path beneath it is
    // DriveRelative too — yet each canonicalizes to a token that is portable
    // exactly because the consumer substitutes its own root. Dropping on the
    // anchor would silently un-key this whole layout; the sibling below is the
    // drive-relative path under NO root, which must still go.
    PathCanon::Layout const layout { .sourceRoot = R"(C:src\proj)", .buildTree = R"(C:src\build)" };
    auto const out = KeySet({ R"(C:src\proj\a.hpp)", R"(C:elsewhere\b.hpp)" }, layout);

    CHECK(out == std::vector<std::string> { "<SRCROOT>/a.hpp" });
}

TEST_CASE("SplitIncludeNotes removes every note line from the hashed text")
{
    // clang-cl reports on STDOUT, the same stream the preprocessed text uses. A
    // note that survived here would be hashed into the cache key as though it were
    // source — and it carries an absolute path, which is exactly what suppressing
    // line markers exists to keep out of a key.
    auto const out = SplitIncludeNotes("int a;\n"
                                       "Note: including file: /home/dev/proj/a.hpp\n"
                                       "int b;\n"
                                       "Note: including file:  /home/dev/proj/b.hpp\n");

    CHECK(out.preprocessed == "int a;\nint b;\n");
    REQUIRE(out.notePaths.size() == 2);
    CHECK(out.notePaths[0] == "/home/dev/proj/a.hpp");
    CHECK(out.notePaths[1] == "/home/dev/proj/b.hpp");
}

TEST_CASE("SplitIncludeNotes recognises an indented note and CRLF endings")
{
    // `cl` indents a note by inclusion depth, so the marker is matched anywhere in
    // the line rather than at its start — exactly as ParseIncludePaths matches it.
    auto const out = SplitIncludeNotes("int a;\r\n"
                                       "  Note: including file:  C:\\src\\proj\\a.hpp\r\n");

    CHECK(out.preprocessed == "int a;\r\n");
    REQUIRE(out.notePaths.size() == 1);
    CHECK(out.notePaths[0] == R"(C:\src\proj\a.hpp)");
}

TEST_CASE("SplitIncludeNotes preserves non-note text byte-for-byte")
{
    // The remainder is hashed, so any normalization here would silently re-key the
    // cache. A final line without a terminator must come back without one too.
    constexpr std::string_view Text = "line one\r\n\nline three";
    auto const out = SplitIncludeNotes(Text);

    CHECK(out.preprocessed == Text);
    CHECK(out.notePaths.empty());
}

TEST_CASE("SplitIncludeNotes leaves a stream that carries no notes alone")
{
    // The GNU drivers report into a depfile and print nothing on either stream, so
    // this is the ordinary POSIX case: the split must be a no-op, not a rewrite.
    auto const out = SplitIncludeNotes("#pragma once\nint x = 1;\n");

    CHECK(out.preprocessed == "#pragma once\nint x = 1;\n");
    CHECK(out.notePaths.empty());
}

TEST_CASE("SplitIncludeNotes keeps a source line that merely contains the marker")
{
    // The input is preprocessed SOURCE, not a pure note stream, and a rule that
    // matched the marker anywhere in the line deleted the whole line from the
    // bytes the key is computed over. Two revisions of a file differing only in
    // such a literal then key identically and the second is served the first's
    // object — a silent wrong build. This repository's own sources contain the
    // literal, so it was reachable while building fastcached itself.
    constexpr std::string_view First = "char const* s = \"Note: including file: /usr/include/aaa.h\";\n";
    constexpr std::string_view Second = "char const* s = \"Note: including file: /usr/include/bbb.h\";\n";

    auto const first = SplitIncludeNotes(First);
    auto const second = SplitIncludeNotes(Second);

    CHECK(first.preprocessed == First);
    CHECK(second.preprocessed == Second);
    CHECK(first.preprocessed != second.preprocessed);
    CHECK(first.notePaths.empty());
    CHECK(second.notePaths.empty());
}

TEST_CASE("KeyDependencySet collapses two spellings of one header into one entry")
{
    // A driver echoes a dependency path as RESOLVED, so `.`/`..` segments from the
    // include search path reach us verbatim. Without normalizing first, one header
    // reached two ways is two key entries — and two machines whose generators spell
    // an include directory differently compute two different keys for identical
    // content, which is a permanent cross-machine miss.
    auto const out = KeySet({ "/home/dev/proj/build/../inc/a.hpp", "/home/dev/proj/./inc/a.hpp" }, PosixLayout());

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet drops a toolchain tree nested under the build tree")
{
    // A vcpkg tree under the build tree canonicalizes, so a root test alone keeps
    // it — but it is the producing machine's spelling of content the compiler
    // identity already covers, and both DirectManifest and the replay guard
    // classify it as toolchain. All three have to agree, or a vcpkg relocation
    // re-keys every preprocessed entry while direct mode keeps hitting.
    PathCanon::Layout const windows { .sourceRoot = R"(D:\Project)", .buildTree = R"(D:\Project\out\build\win64)" };
    auto const out = KeySet({ R"(D:\Project\out\build\win64\vcpkg_installed\x64-windows\include\zlib.h)",
                              R"(D:\Project\out\build\win64\generated\config.hpp)" },
                            windows);

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<BUILDTREE>/generated/config.hpp");
}
