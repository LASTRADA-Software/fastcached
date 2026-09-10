// SPDX-License-Identifier: Apache-2.0
#include "PathResolve.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// Create a directory tree with a file in it, and return the file's path.
/// Create a file under `scratch` and name it back.
///
/// Through `ScratchDirectory::Write`, which creates the parents and THROWS when the
/// open or the close fails. What stood here opened a bare `ofstream` and observed
/// nothing, so a fixture that silently wrote no file would leave these cases
/// resolving a path that does not exist -- and two of them assert about resolution
/// SUCCEEDING, which is the direction that would then pass for the wrong reason.
[[nodiscard]] std::filesystem::path MakeFile(FastCache::Testing::ScratchDirectory const& scratch, std::string_view relative)
{
    scratch.Write(relative);
    return scratch / relative;
}

/// Compare two paths as the filesystem understands them, so a test does not fail
/// on a separator or a `/private` prefix macOS adds to its temp directory.
[[nodiscard]] bool SameFile(std::string_view lhs, std::string_view rhs)
{
    std::error_code ec;
    return std::filesystem::equivalent(std::filesystem::path { lhs }, std::filesystem::path { rhs }, ec) && !ec;
}

} // namespace

TEST_CASE("A relative path is returned verbatim")
{
    // By contract, and load-bearing: a relative dependency path resolves against
    // the compile's working directory, so it is ALREADY machine-independent and is
    // kept in the key as-is. Absolutizing it here would either re-key it for no
    // reason or, when the working directory lies under neither root, push it
    // outside both and have KeyDependencySet drop it altogether.
    auto const resolver = MakePathResolver();
    CHECK(resolver->Resolve("inc/a.hpp") == "inc/a.hpp");
    CHECK(resolver->ResolveDirectory("inc") == "inc");
}

TEST_CASE("An empty path resolves to itself without touching the filesystem")
{
    auto const resolver = MakePathResolver();
    CHECK(resolver->Resolve("").empty());
    CHECK(resolver->ResolveDirectory("").empty());
    CHECK(resolver->FilesystemCalls() == 0);
}

TEST_CASE("A path that does not exist comes back usable rather than empty")
{
    // The launcher must never lose a path to a failed probe: a build tree that has
    // not been created yet, or an argument that was never a path, has to survive
    // this call unchanged in every way that matters to a prefix test.
    auto const resolver = MakePathResolver();
    // `UniqueScratchPath` creates nothing, which is exactly what a case about an
    // absent path wants -- and it cannot be made to exist by a leftover from an
    // older run, which a fixed name could.
    auto const absent = (FastCache::Testing::UniqueScratchPath("fc-resolve-absent") / "nope.hpp").string();
    auto const resolved = resolver->Resolve(absent);
    CHECK_FALSE(resolved.empty());
    CHECK(std::filesystem::path { resolved }.filename() == "nope.hpp");
}

TEST_CASE("Two spellings of one directory resolve to the same answer")
{
    // The property the whole module exists for. A symlink is the portable stand-in
    // for the 8.3 short name measured on Windows: same directory, two spellings,
    // and a string prefix comparison that cannot tell they are the same file.
    FastCache::Testing::ScratchDirectory const scratch { "fc-resolve-alias" };
    auto const real = scratch / "real";
    auto const file = MakeFile(scratch, "real/inc/h1.h");

    std::error_code ec;
    auto const link = scratch / "link";
    std::filesystem::create_directory_symlink(real, link, ec);
    if (ec)
        SKIP("this host does not permit creating symlinks; nothing to compare");

    auto const resolver = MakePathResolver();
    auto const viaReal = resolver->Resolve(file.string());
    auto const viaLink = resolver->Resolve((link / "inc" / "h1.h").string());
    CHECK(viaReal == viaLink);
    CHECK(SameFile(viaReal, file.string()));
}

TEST_CASE("A root's own final component is resolved, unlike a file's")
{
    // The reason ResolveDirectory exists. Resolve() memoizes the PARENT and appends
    // the leaf, which is what keeps it cheap for the hundreds of headers a TU
    // reports; a layout root passed through it would keep whatever spelling its
    // last component had — and a root whose last component is the aliased one is
    // exactly the shape issue #66 describes.
    FastCache::Testing::ScratchDirectory const scratch { "fc-resolve-rootleaf" };
    auto const real = scratch / "real";
    std::error_code ec;
    std::filesystem::create_directories(real, ec);
    auto const link = scratch / "link";
    std::filesystem::create_directory_symlink(real, link, ec);
    if (ec)
        SKIP("this host does not permit creating symlinks; nothing to compare");

    auto const resolver = MakePathResolver();
    CHECK(resolver->ResolveDirectory(link.string()) == resolver->ResolveDirectory(real.string()));
}

TEST_CASE("Resolution is memoized per directory, not per path")
{
    // The cost answer issue #66 asks for. A real translation unit reports ~635
    // headers from a few dozen directories, so per-path resolution is only
    // affordable if the filesystem is asked once per directory. Ten files in one
    // directory must not be ten probes.
    FastCache::Testing::ScratchDirectory const scratch { "fc-resolve-memo" };
    auto const directory = scratch / "inc";
    for (int index = 0; index < 10; ++index)
        (void) MakeFile(scratch, "inc/h" + std::to_string(index) + ".h");

    auto const resolver = MakePathResolver();
    for (int index = 0; index < 10; ++index)
        (void) resolver->Resolve((directory / ("h" + std::to_string(index) + ".h")).string());

    auto const afterTen = resolver->FilesystemCalls();
    CHECK(afterTen <= 2); // one successful step, plus at most one failed step before it

    // And a repeat costs nothing at all.
    (void) resolver->Resolve((directory / "h0.h").string());
    CHECK(resolver->FilesystemCalls() == afterTen);
}

TEST_CASE("Resolution is idempotent")
{
    // RecordManifest reconciles its own inputs even though its caller already did,
    // which is only safe because resolving an already-resolved path is a no-op.
    FastCache::Testing::ScratchDirectory const scratch { "fc-resolve-idempotent" };
    auto const file = MakeFile(scratch, "inc/h1.h");

    auto const resolver = MakePathResolver();
    auto const once = resolver->Resolve(file.string());
    CHECK(resolver->Resolve(once) == once);
}
