// SPDX-License-Identifier: Apache-2.0
#include "ReplayGuard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

PathCanon::Layout PosixLayout()
{
    return { .sourceRoot = "/home/dev/proj", .buildTree = "/home/dev/proj/build" };
}

PathCanon::Layout WindowsLayout()
{
    return { .sourceRoot = R"(D:\Project)", .buildTree = R"(D:\Project\out\build\win64)" };
}

/// A stored value's regions in their positional order: 0 = stdout, 1 = stderr,
/// 2 = the GNU depfile. Mirrors what the launcher stores, so a fixture here cannot
/// drift from the shape the guard actually sees.
std::vector<TextRegion> Value(std::string stdoutText, std::string stderrText, std::string depfile = {})
{
    std::vector<TextRegion> regions {
        { .grammar = PathCanon::Grammar::ShowIncludes, .bytes = std::move(stdoutText) },
        { .grammar = PathCanon::Grammar::ShowIncludes, .bytes = std::move(stderrText) },
    };
    if (!depfile.empty())
        regions.push_back({ .grammar = PathCanon::Grammar::GccDepfile, .bytes = std::move(depfile) });
    return regions;
}

/// A scratch directory that removes itself, so a failing assertion cannot leave a
/// tree behind that makes the next run pass for the wrong reason.
struct ScopedTree
{
    std::filesystem::path base;
    std::filesystem::path root;

    explicit ScopedTree(std::string_view name):
        // A unique PARENT with the caller's name hung under it, rather than the name
        // alone. The name is what a reader recognises and one of them is itself a
        // nested path, so it stays exactly as written; what changes is that it can no
        // longer be the whole story. `temp / "<fixed>"` is the same directory in every
        // concurrent test process -- see `tests/ScratchPath.hpp` for the five times
        // that has been paid for.
        base { FastCache::Testing::UniqueScratchPath("fc-replayguard") },
        root { base / name }
    {
        std::filesystem::create_directories(root);
    }

    ~ScopedTree()
    {
        // The BASE, not the root: the root may be nested inside it.
        std::error_code ignored;
        std::filesystem::remove_all(base, ignored);
    }

    ScopedTree(ScopedTree const&) = delete;
    ScopedTree(ScopedTree&&) = delete;
    ScopedTree& operator=(ScopedTree const&) = delete;
    ScopedTree& operator=(ScopedTree&&) = delete;

    /// Create `relative` with some content, and return its absolute path.
    [[nodiscard]] std::string Write(std::string_view relative) const
    {
        auto const path = root / relative;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream { path } << "#pragma once\n";
        return path.string();
    }
};

} // namespace

// --- ReplayedDependencyPaths: which paths a replay is answerable for ----------

TEST_CASE("A depfile's dependencies are checked but its rule target is not", "[replay-guard]")
{
    // The target is the object file, which does not exist yet on the path that
    // calls this — probing it would turn every hit into a miss.
    auto const regions = Value("", "", "/home/dev/proj/build/a.o: /home/dev/proj/a.cpp /home/dev/proj/hdr.hpp\n");
    auto const paths = ReplayedDependencyPaths(regions, PosixLayout());
    CHECK(paths == std::vector<std::string> { "/home/dev/proj/a.cpp", "/home/dev/proj/hdr.hpp" });
}

TEST_CASE("A dependency outside every root rides the toolchain stamp", "[replay-guard]")
{
    // Probing it would break convergence between machines with different system
    // include prefixes, not merely cost one hit.
    auto const regions = Value("", "", "build/a.o: /home/dev/proj/a.cpp /usr/include/c++/15/string\n");
    auto const paths = ReplayedDependencyPaths(regions, PosixLayout());
    CHECK(paths == std::vector<std::string> { "/home/dev/proj/a.cpp" });
}

TEST_CASE("A vcpkg tree nested inside the build tree is still toolchain content", "[replay-guard]")
{
    // Exercises the marker branch of IsToolchainHeader rather than the root branch:
    // this path IS under a root, and must still be skipped.
    auto const regions = Value("",
                               "",
                               R"(a.obj: D:\Project\src\a.cpp D:\Project\out\build\win64\vcpkg_installed\x64\zstd.h)"
                               "\n");
    auto const paths = ReplayedDependencyPaths(regions, WindowsLayout());
    CHECK(paths == std::vector<std::string> { R"(D:\Project\src\a.cpp)" });
}

TEST_CASE("A relative dependency is checked, not mistaken for toolchain content", "[replay-guard]")
{
    // IsToolchainHeader reports every relative path as outside the roots, so a
    // filter that asked it first would skip all of them — and a build whose
    // compiler emits relative dependency paths would lose the guard entirely.
    auto const regions = Value("", "", "build/a.o: ../../src/a.cpp ../../src/hdr.hpp\n");
    auto const paths = ReplayedDependencyPaths(regions, PosixLayout());
    CHECK(paths == std::vector<std::string> { "../../src/a.cpp", "../../src/hdr.hpp" });
}

TEST_CASE("The phony rules -MP emits contribute no dependencies", "[replay-guard]")
{
    auto const regions = Value("",
                               "",
                               "build/a.o: /home/dev/proj/a.cpp /home/dev/proj/hdr.hpp\n"
                               "\n"
                               "/home/dev/proj/hdr.hpp:\n");
    auto const paths = ReplayedDependencyPaths(regions, PosixLayout());
    CHECK(paths == std::vector<std::string> { "/home/dev/proj/a.cpp", "/home/dev/proj/hdr.hpp" });
}

TEST_CASE("showIncludes notes are a dependency record too", "[replay-guard]")
{
    // Ninja parses these as `deps = msvc`, so a stale note is the same
    // never-converging build a stale depfile is.
    std::string const notes = std::string { R"(Note: including file: D:\Project\src\a.hpp)" } + "\r\n"
                              + R"(Note: including file:  C:\Program Files\Windows Kits\10\um\windows.h)" + "\r\n";
    // clang-cl emits them on stdout, cl on stderr; both regions carry the grammar.
    auto const paths = ReplayedDependencyPaths(Value("", notes), WindowsLayout());
    CHECK(paths == std::vector<std::string> { R"(D:\Project\src\a.hpp)" });
}

TEST_CASE("A diagnostics region declares no dependencies", "[replay-guard]")
{
    // Pins the deliberate omission from the grammar table: a diagnostic quotes a
    // path, it does not declare a dependency on it.
    std::vector<TextRegion> const regions {
        { .grammar = PathCanon::Grammar::MsvcDiagnostics,
          .bytes = R"(D:\Project\src\gone.hpp(12,3): warning C4100: unreferenced)"
                   "\r\n" },
    };
    CHECK(ReplayedDependencyPaths(regions, WindowsLayout()).empty());
}

TEST_CASE("A span the region walker declined to localize is not probed", "[replay-guard]")
{
    // ParseIncludePaths finds its marker anywhere on a line; PathCanon::SplitLine
    // requires it at column 0. A line only the first accepts arrives still bearing
    // its canonical token, which names nothing on any machine.
    auto const regions = Value("",
                               R"(a.cpp(3,1): note: Note: including file: <SRCROOT>/src/a.hpp)"
                               "\r\n");
    CHECK(ReplayedDependencyPaths(regions, WindowsLayout()).empty());
}

TEST_CASE("A value stored before depfile support asserts nothing", "[replay-guard]")
{
    // Two regions only, and a GNU compile puts no include notes on either stream.
    auto const regions = Value("", "warning: unused variable 'x'\n");
    CHECK(ReplayedDependencyPaths(regions, PosixLayout()).empty());
}

TEST_CASE("A header included many times is reported once", "[replay-guard]")
{
    std::string const notes = std::string { R"(Note: including file: D:\Project\src\a.hpp)" } + "\r\n"
                              + R"(Note: including file:  D:\Project\src\b.hpp)" + "\r\n"
                              + R"(Note: including file:   D:\Project\src\a.hpp)" + "\r\n";
    auto const paths = ReplayedDependencyPaths(Value(notes, ""), WindowsLayout());
    CHECK(paths == std::vector<std::string> { R"(D:\Project\src\a.hpp)", R"(D:\Project\src\b.hpp)" });
}

TEST_CASE("A Windows layout classifies paths the same way on any host", "[replay-guard]")
{
    // std::filesystem::path::is_absolute() answers this from the HOST, and would
    // call every one of these relative on POSIX — sending absolute Windows paths
    // down the relative branch, to be probed against the working directory.
    auto const regions = Value("",
                               "",
                               R"(a.obj: D:\Project\src\a.cpp src\rel.hpp C:\WindowsKits\10\um\x.h)"
                               "\n");
    auto const paths = ReplayedDependencyPaths(regions, WindowsLayout());
    CHECK(paths == std::vector<std::string> { R"(D:\Project\src\a.cpp)", R"(src\rel.hpp)" });
}

TEST_CASE("A drive-relative dependency is not probed, but a relative one is", "[replay-guard]")
{
    // The two must not collapse into one branch. `src\rel.hpp` resolves against
    // the compile's working directory, which is also the launcher's, so it is this
    // machine's path and is checked. `C:foo\bar.hpp` resolves against drive C's own
    // current directory, which std::filesystem::operator/ reaches by no route: on a
    // POSIX host the join is a name that exists nowhere, so every hit carrying such
    // a path would be discarded forever. A path that cannot be examined counts as
    // present (issue #65).
    auto const regions = Value("",
                               "",
                               R"(a.obj: C:foo\bar.hpp src\rel.hpp)"
                               "\n");
    auto const paths = ReplayedDependencyPaths(regions, WindowsLayout());
    CHECK(paths == std::vector<std::string> { R"(src\rel.hpp)" });
}

TEST_CASE("A drive-relative dependency is skipped even under a drive-relative root", "[replay-guard]")
{
    // The guard parts company with the key filter here, and deliberately. That one
    // needs a portable SPELLING, which a drive-relative root can still supply, so
    // it keeps this path. This one needs something to stat, and a path anchored to
    // drive C's current directory is not that under any working directory it could
    // be handed — so probing it would discard every hit carrying it, forever.
    PathCanon::Layout const layout { .sourceRoot = R"(C:src\proj)", .buildTree = R"(C:src\build)" };
    auto const regions = Value("",
                               "",
                               R"(a.obj: C:src\proj\a.hpp rel.hpp)"
                               "\n");
    auto const paths = ReplayedDependencyPaths(regions, layout);
    CHECK(paths == std::vector<std::string> { "rel.hpp" });
}

// --- MissingReplayedDependency: the filesystem probe --------------------------

TEST_CASE("A replay whose dependencies all exist is honoured", "[replay-guard]")
{
    ScopedTree const tree { "fc-replayguard-present" };
    auto const header = tree.Write("src/inc/Hdr.hpp");
    auto const source = tree.Write("src/a.cpp");
    PathCanon::Layout const layout { .sourceRoot = (tree.root / "src").string(),
                                     .buildTree = (tree.root / "build").string() };

    auto const regions = Value("", "", "build/a.o: " + source + " " + header + "\n");
    CHECK_FALSE(MissingReplayedDependency(regions, layout, tree.root).has_value());
}

TEST_CASE("A moved header makes the recorded depfile untrue", "[replay-guard]")
{
    // Issue #53. The header's CONTENTS do not change, so the preprocessed text and
    // therefore the object key are identical and the value is served — but the
    // depfile it carries names a path that is now empty.
    ScopedTree const tree { "fc-replayguard-moved" };
    auto const source = tree.Write("src/a.cpp");
    auto const oldPath = tree.Write("src/inc/old/Hdr.hpp");
    PathCanon::Layout const layout { .sourceRoot = (tree.root / "src").string(),
                                     .buildTree = (tree.root / "build").string() };
    auto const regions = Value("", "", "build/a.o: " + source + " " + oldPath + "\n");

    REQUIRE_FALSE(MissingReplayedDependency(regions, layout, tree.root).has_value());

    std::filesystem::create_directories(tree.root / "src" / "inc" / "new");
    std::filesystem::rename(oldPath, tree.root / "src" / "inc" / "new" / "Hdr.hpp");

    auto const missing = MissingReplayedDependency(regions, layout, tree.root);
    REQUIRE(missing.has_value());
    // Compared as an optional rather than dereferenced: the equality is the same,
    // and clang-tidy cannot see a Catch2 REQUIRE as the guard on an access.
    CHECK(missing == oldPath);
}

TEST_CASE("An absent path outside the roots does not discard the hit", "[replay-guard]")
{
    // Cross-machine sharing depends on this: the producer's system headers are not
    // this machine's, and requiring them would make both machines miss forever.
    ScopedTree const tree { "fc-replayguard-foreign" };
    auto const source = tree.Write("src/a.cpp");
    PathCanon::Layout const layout { .sourceRoot = (tree.root / "src").string(),
                                     .buildTree = (tree.root / "build").string() };

    auto const regions = Value("", "", "build/a.o: " + source + " /opt/nonexistent-sdk/include/thing.h\n");
    CHECK_FALSE(MissingReplayedDependency(regions, layout, tree.root).has_value());
}

TEST_CASE("A relative dependency resolves against the supplied working directory", "[replay-guard]")
{
    // Not against the process working directory: the launcher passes the compile's
    // own, and a test must be able to say which without mutating global state.
    ScopedTree const tree { "fc-replayguard-relative" };
    (void) tree.Write("src/hdr.hpp");
    PathCanon::Layout const layout { .sourceRoot = (tree.root / "src").string(),
                                     .buildTree = (tree.root / "build").string() };

    auto const present = Value("", "", "build/a.o: src/hdr.hpp\n");
    CHECK_FALSE(MissingReplayedDependency(present, layout, tree.root).has_value());

    auto const absent = Value("", "", "build/a.o: src/gone.hpp\n");
    auto const missing = MissingReplayedDependency(absent, layout, tree.root);
    REQUIRE(missing.has_value());
    CHECK(missing == "src/gone.hpp");
}

TEST_CASE("A value with no regions at all asserts nothing", "[replay-guard]")
{
    CHECK_FALSE(MissingReplayedDependency({}, PosixLayout(), std::filesystem::temp_directory_path()).has_value());
}

TEST_CASE("A replayed dependency this host cannot read discards the hit rather than the build", "[replay-guard]")
{
    // The one input on this path that arrives over the NETWORK: these bytes were
    // written by whichever machine produced the entry, so a legacy spelling from a
    // POSIX producer -- or from a launcher built before the UTF-8 code page -- lands
    // on a consumer that reads narrow bytes as UTF-8 and cannot decode them. There
    // the `std::filesystem::path` CONSTRUCTOR throws, ahead of every `error_code`
    // this function carefully takes, and an uncaught throw out of a cache lookup
    // fails a build the compiler would have completed.
    //
    // Reported as missing, which discards the hit and recompiles: a dependency this
    // host cannot name is one it cannot check, and an unchecked hit is what this
    // guard exists to refuse.
    ScopedTree const tree { "fc-replayguard-unreadable" };
    (void) tree.Write("src/hdr.hpp");
    PathCanon::Layout const layout { .sourceRoot = (tree.root / "src").string(),
                                     .buildTree = (tree.root / "build").string() };

    constexpr auto legacyName = "src/gr\xFC"
                                "n.hpp";
    auto const regions = Value("", "", std::string { "build/a.o: " } + legacyName + "\n");

    std::optional<std::string> missing;
    CHECK_NOTHROW(missing = MissingReplayedDependency(regions, layout, tree.root));
    REQUIRE(missing.has_value());
    CHECK(missing == legacyName);
}
