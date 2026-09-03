// SPDX-License-Identifier: Apache-2.0
#include "PathResolve.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// A scratch directory that removes itself, so a failing assertion cannot leave a
/// tree behind for the next run to trip over.
class ScratchTree
{
  public:
    explicit ScratchTree(std::string_view name):
        // A unique PARENT with the caller's name hung under it, rather than the name
        // alone. The name is what a reader recognises and one of them is itself a
        // nested path, so it stays exactly as written; what changes is that it can no
        // longer be the whole story. `temp / "<fixed>"` is the same directory in every
        // concurrent test process -- see `tests/ScratchPath.hpp` for the five times
        // that has been paid for.
        _base { FastCache::Testing::UniqueScratchPath("fc-resolve") },
        _root { _base / std::filesystem::path { std::string { name } } }
    {
        std::error_code ec;
        std::filesystem::create_directories(_root, ec);
    }

    ~ScratchTree()
    {
        // The BASE, not the root: the root may be nested inside it.
        std::error_code ec;
        std::filesystem::remove_all(_base, ec);
    }

    ScratchTree(ScratchTree const&) = delete;
    ScratchTree& operator=(ScratchTree const&) = delete;
    ScratchTree(ScratchTree&&) = delete;
    ScratchTree& operator=(ScratchTree&&) = delete;

    [[nodiscard]] std::filesystem::path const& Root() const noexcept
    {
        return _root;
    }

  private:
    std::filesystem::path _base;
    std::filesystem::path _root;
};

/// Create a directory tree with a file in it, and return the file's path.
[[nodiscard]] std::filesystem::path MakeFile(std::filesystem::path const& directory, std::string_view name)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    auto const file = directory / std::filesystem::path { std::string { name } };
    std::ofstream { file } << "x\n";
    return file;
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
    ScratchTree const scratch { "alias" };
    auto const real = scratch.Root() / "real";
    auto const file = MakeFile(real / "inc", "h1.h");

    std::error_code ec;
    auto const link = scratch.Root() / "link";
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
    ScratchTree const scratch { "rootleaf" };
    auto const real = scratch.Root() / "real";
    std::error_code ec;
    std::filesystem::create_directories(real, ec);
    auto const link = scratch.Root() / "link";
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
    ScratchTree const scratch { "memo" };
    auto const directory = scratch.Root() / "inc";
    for (int index = 0; index < 10; ++index)
        (void) MakeFile(directory, "h" + std::to_string(index) + ".h");

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
    ScratchTree const scratch { "idempotent" };
    auto const file = MakeFile(scratch.Root() / "inc", "h1.h");

    auto const resolver = MakePathResolver();
    auto const once = resolver->Resolve(file.string());
    CHECK(resolver->Resolve(once) == once);
}
