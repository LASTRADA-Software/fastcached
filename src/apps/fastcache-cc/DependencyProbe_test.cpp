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

/// Where a compile in that layout runs, and so what a relative dependency path
/// resolves against: the build tree, which is where every generator this cares
/// about spawns the compiler.
constexpr std::string_view PosixWorkingDirectory = "/home/dev/proj/build";

std::vector<std::string> KeySet(std::vector<std::string> const& raw,
                                PathCanon::Layout const& layout,
                                std::string_view workingDirectory = PosixWorkingDirectory)
{
    return KeyDependencySet(std::span<std::string const> { raw }, layout, workingDirectory);
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

TEST_CASE("KeyDependencySet drops a toolchain reached through a relative include path")
{
    // The same exclusion, reached the other way (issue #64). `-isystem
    // ../toolchain/include` makes the driver report `../toolchain/include/foo.h`,
    // which lies under no root — so a filter that asked whether the path was
    // ABSOLUTE kept and hashed the very file it drops when the driver spells it
    // `/home/dev/toolchain/include/foo.h`. Both spellings name the same file, and
    // the answer must not depend on which one arrived.
    auto const relative = KeySet({ "../../toolchain/include/foo.h" }, PosixLayout());
    auto const absolute = KeySet({ "/home/dev/toolchain/include/foo.h" }, PosixLayout());

    CHECK(relative.empty());
    CHECK(relative == absolute);
}

TEST_CASE("KeyDependencySet resolves a relative path and keeps it as a token")
{
    // A relative path is classified by what it RESOLVES to, not by its spelling:
    // resolved, a project header reached relatively is the same `<SRCROOT>`/
    // `<BUILDTREE>` token its absolute spelling would have produced, which is what
    // makes the set say which files the compile read rather than how the driver
    // happened to write them down.
    auto const out = KeySet({ "gen/Local.hpp", "../inc/Shared.hpp" }, PosixLayout());

    REQUIRE(out.size() == 2);
    CHECK(out[0] == "<BUILDTREE>/gen/Local.hpp");
    CHECK(out[1] == "<SRCROOT>/inc/Shared.hpp");
}

TEST_CASE("KeyDependencySet keys one header identically however the driver spelled it")
{
    // The same header reached through an absolute include directory and through a
    // relative one is ONE dependency. Keeping the relative spelling made it two —
    // the defect NormalizePath exists to prevent, one level up.
    auto const out = KeySet({ "/home/dev/proj/inc/a.hpp", "../inc/a.hpp" }, PosixLayout());

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet resolves under the layout's conventions, not the host's")
{
    // std::filesystem recognises `\` as a separator only on a Windows HOST, so a
    // Windows path normalized on POSIX keeps every `..` segment — and a `..` that
    // survives into Canonicalize produces a token naming a file the path does not
    // name: `C:\src\proj\out\build\..\..\inc\a.hpp` still prefix-matches the build
    // tree, so it comes back as `<BUILDTREE>/../../inc/a.hpp` rather than as the
    // source-root header it is. That is a key input that differs between two hosts
    // for one input, which is the one thing a shared key may never do.
    //
    // All three spellings below name `C:\src\proj\inc\a.hpp` — absolute with a `..`
    // to collapse, relative from the build tree, and mixed separators — so they are
    // ONE entry; the vendored SDK resolves outside both roots and is dropped exactly
    // as its absolute spelling would be.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\out\build)" };
    auto const out = KeySet({ R"(C:\src\proj\out\build\..\..\inc\a.hpp)",
                              R"(..\..\inc\a.hpp)",
                              "../../inc/a.hpp",
                              R"(..\..\..\vendor\sdk\include\foo.h)" },
                            windows,
                            R"(C:\src\proj\out\build)");

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet keeps a UNC root across the lexical pass")
{
    // Two leading separators are a UNC root, which a Windows host preserves as a
    // root name and a POSIX host collapses to one `/` -- measured on libc++, where
    // `//server/share/proj/../inc/a.hpp` normalizes to `/server/share/inc/a.hpp`.
    // Left alone, a UNC-rooted layout evaluated on POSIX stops prefix-matching its
    // own root, every project header is classified toolchain, and the whole
    // dependency set comes back empty: a key input that depends on which host
    // computed it, which is the one thing a shared key may never do.
    PathCanon::Layout const unc { .sourceRoot = R"(\\build01\src\proj)", .buildTree = R"(\\build01\src\proj\build)" };
    auto const out =
        KeySet({ R"(\\build01\src\proj\build\..\inc\a.hpp)", R"(..\inc\a.hpp)" }, unc, R"(\\build01\src\proj\build)");

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet decides absoluteness before collapsing dot segments")
{
    // Order matters in exactly one shape. `C:/../x` normalizes to a bare `x` on a
    // POSIX host, because `C:` is an ordinary filename there for `..` to eat --
    // Windows cannot ascend past a drive root and keeps `C:/x`. Collapsing first
    // therefore turns an absolute path into a relative one on one host only, and
    // it would then be resolved against the working directory and keyed as a
    // build-tree header that has nothing to do with it. The path is absolute, lies
    // under neither root, and must be dropped as toolchain on every host.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(C:\..\x\vector)" }, windows, R"(C:\src\proj\build)");

    CHECK(out.empty());
}

TEST_CASE("KeyDependencySet collapses a relative path's separators")
{
    // Two spellings of one relative path are one dependency, whichever separator
    // the driver used — `cl` and clang-cl disagree about this within a single build.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(inc\a.hpp)", "inc/a.hpp" }, windows, R"(C:\src\proj\build)");

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<BUILDTREE>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet drops a relative path when there is no working directory")
{
    // Without one there is nothing to resolve against, and a path this machine
    // cannot classify is a path it cannot hash portably — hashing it anyway is the
    // whole defect. The launcher's `dependency set: 0 of N` note is what keeps the
    // drop visible; an absolute path in the same set is unaffected, which is what
    // distinguishes this from a probe that reported nothing.
    auto const out = KeySet({ "../inc/a.hpp", "/home/dev/proj/inc/b.hpp" }, PosixLayout(), "");

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/b.hpp");
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
    // The working directory is spelled the layout's way rather than left to the
    // POSIX default: nothing here is relative, so it is inert either way, but a
    // POSIX cwd under a Windows layout is a combination no machine ever presents
    // and a case added later would silently inherit it.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out =
        KeySet({ R"(C:\src\proj\inc\a.hpp)", R"(C:\Program Files\MSVC\include\vector)" }, windows, windows.buildTree);

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/inc/a.hpp");
}

TEST_CASE("KeyDependencySet drops a drive-relative path but keeps a relative one")
{
    // The two must not collapse into one branch. `src\rel.hpp` resolves against the
    // compile's working directory and names the same file on every machine running
    // the same build, so it is resolved and keyed — as the token its absolute
    // spelling would have produced, since issue #64 (the expectation moved with
    // that change; the property this case exists for did not). `C:foo\bar.hpp`
    // resolves against drive C's own current directory — per-process state on the
    // producing machine that no cache entry records — so resolving it the same way
    // would name a file that was never read, and keying its spelling would let two
    // machines whose C: cwd differs produce the SAME key for DIFFERENT headers.
    // Before issue #65 the classifier reported it as absolute, which dropped it
    // too, but by way of an answer that was not true.
    PathCanon::Layout const windows { .sourceRoot = R"(C:\src\proj)", .buildTree = R"(C:\src\proj\build)" };
    auto const out = KeySet({ R"(C:foo\bar.hpp)", R"(src\rel.hpp)", "C:" }, windows, R"(C:\src\proj\build)");

    CHECK(out == std::vector<std::string> { "<BUILDTREE>/src/rel.hpp" });
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
    auto const out = KeySet({ R"(C:src\proj\a.hpp)", R"(C:elsewhere\b.hpp)" }, layout, R"(C:src\build)");

    CHECK(out == std::vector<std::string> { "<SRCROOT>/a.hpp" });
}

TEST_CASE("KeyDependencySet is insensitive to where a vendored tree sits")
{
    // The regression (issue #64). A vendored or relocatable toolchain reached
    // through a relative include path — `-I../../vendor/sdk/include`, the ordinary
    // way one is referenced from a build directory — comes back from the driver as
    // a relative dependency path. Classified by its SPELLING it lies under no root,
    // so it is kept and hashed ahead of the toolchain test that drops the very same
    // file when the driver spells it absolutely. Two machines whose build directory
    // sits at a different depth then key every translation unit that touches it
    // differently: the "two machines with the same compiler share nothing at all"
    // outcome the absolute-path exclusion exists to prevent, arrived at from the
    // other side.
    PathCanon::Layout const shallow { .sourceRoot = "/a/proj", .buildTree = "/a/proj/build" };
    PathCanon::Layout const deep { .sourceRoot = "/b/work/proj", .buildTree = "/b/work/proj/out/build/x64" };

    auto const fromShallow = KeySet({ "../../vendor/sdk/include/foo.h", "../inc/a.hpp" }, shallow, shallow.buildTree);
    auto const fromDeep = KeySet({ "../../../../../vendor/sdk/include/foo.h", "../../../inc/a.hpp" }, deep, deep.buildTree);

    // Both name the same two files: the project header, which is the whole reason
    // this set exists, and the vendored toolchain header, which is covered by the
    // compiler identity already in the key.
    REQUIRE(fromShallow.size() == 1);
    CHECK(fromShallow[0] == "<SRCROOT>/inc/a.hpp");
    CHECK(fromShallow == fromDeep);
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
                            windows,
                            windows.buildTree);

    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<BUILDTREE>/generated/config.hpp");
}
