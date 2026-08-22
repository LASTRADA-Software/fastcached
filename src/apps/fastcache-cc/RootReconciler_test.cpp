// SPDX-License-Identifier: Apache-2.0
#include "RootReconciler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// A resolver driven by a table instead of a filesystem.
///
/// The whole point of the seam: the conditions this class exists for — an 8.3
/// short component, a `subst` drive, a junction — cannot be created on the host
/// running these tests, and two of the three cannot be created on any host that is
/// not Windows. A table states the aliasing directly and every case below is then
/// reproducible everywhere.
class FakeResolver final: public IPathResolver
{
  public:
    /// @param aliases Prefix -> the spelling the "filesystem" reports for it.
    explicit FakeResolver(std::map<std::string, std::string, std::less<>> aliases):
        _aliases { std::move(aliases) }
    {
    }

    std::string Resolve(std::string_view path) override
    {
        ++_calls;
        return Rewrite(path);
    }

    std::string ResolveDirectory(std::string_view path) override
    {
        ++_calls;
        return Rewrite(path);
    }

    [[nodiscard]] std::size_t FilesystemCalls() const noexcept override
    {
        return _calls;
    }

  private:
    [[nodiscard]] std::string Rewrite(std::string_view path) const
    {
        for (auto const& [from, to]: _aliases)
            if (path.starts_with(from))
                return to + std::string { path.substr(from.size()) };
        return std::string { path };
    }

    std::map<std::string, std::string, std::less<>> _aliases;
    std::size_t _calls { 0 };
};

/// The measured Windows shape: the build system spells the root short, `cl`
/// resolves an include through the filesystem and reports it long.
[[nodiscard]] FakeResolver ShortNameHost()
{
    return FakeResolver { { { R"(C:\Users\RUNNER~1\p)", R"(C:\Users\runneradmin\p)" } } };
}

} // namespace

TEST_CASE("WithoutTrailingSeparator trims a root but never a bare one")
{
    CHECK(WithoutTrailingSeparator("/x/build/") == "/x/build");
    CHECK(WithoutTrailingSeparator("/x/build//") == "/x/build");
    CHECK(WithoutTrailingSeparator(R"(D:\proj\build\)") == R"(D:\proj\build)");
    CHECK(WithoutTrailingSeparator("/x/build") == "/x/build");

    // A bare root IS its trailing separator; trimming `C:\` to `C:` would also
    // flip the separator style PathCanon derives from it when localizing, and `/`
    // would become empty, which is a prefix of nothing at all.
    CHECK(WithoutTrailingSeparator("/") == "/");
    CHECK(WithoutTrailingSeparator(R"(C:\)") == R"(C:\)");
    CHECK(WithoutTrailingSeparator("C:/") == "C:/");
    CHECK(WithoutTrailingSeparator("").empty());

    // The drive test is narrow, so a three-byte string that merely has a colon in
    // the middle is an ordinary root and its separator comes off.
    CHECK(WithoutTrailingSeparator("a:/") == "a:/"); // a real drive letter
    CHECK(WithoutTrailingSeparator("1:/") == "1:");  // not a drive at all
    CHECK(WithoutTrailingSeparator("ab/") == "ab");
}

TEST_CASE("A path the driver spelled differently is translated into the build's spelling")
{
    // The defect issue #66 records: the root is spelled one way and nothing the
    // driver emits shares that spelling, so every root test fails silently.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    CHECK(reconciler.Path(R"(C:\Users\runneradmin\p\src\inc\h1.h)") == R"(C:\Users\RUNNER~1\p\src\inc\h1.h)");
    CHECK(reconciler.Directory(R"(C:\Users\runneradmin\p\src\inc)") == R"(C:\Users\RUNNER~1\p\src\inc)");
}

TEST_CASE("A path already spelled the build's way is returned untouched and costs no probe")
{
    // The correctness case, not an optimization. Resolution rewrites a symlink
    // ANYWHERE in a path, so round-tripping an already-correct one would rewrite
    // an in-tree alias and split the key between two byte-identical checkouts.
    auto resolver = FakeResolver { { { R"(C:\p\src\inc)", R"(C:\p\src\real-inc)" } } };
    RootReconciler reconciler { R"(C:\p\src)", R"(C:\p\build)", resolver };

    // Measured from AFTER construction: the two roots are resolved once there, and
    // that is the only filesystem work a healthy build should ever pay for.
    auto const afterConstruction = resolver.FilesystemCalls();

    constexpr auto viaInTreeAlias = R"(C:\p\src\inc\h1.h)";
    CHECK(reconciler.Path(viaInTreeAlias) == viaInTreeAlias);
    CHECK(reconciler.Directory(R"(C:\p\src\inc)") == R"(C:\p\src\inc)");
    CHECK(resolver.FilesystemCalls() == afterConstruction);
}

TEST_CASE("A path under neither root keeps its exact bytes")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    // A toolchain header is covered by the compiler identity, and rewriting its
    // spelling would put this machine's install prefix into a stored value.
    constexpr auto sdk = R"(C:\Program Files (x86)\Windows Kits\10\um\windows.h)";
    CHECK(reconciler.Path(sdk) == sdk);
}

TEST_CASE("A trailing separator on a root does not double in the translated path")
{
    // Untrimmed, the as-given test fails (IsSegmentPrefix wants a separator AFTER
    // the root), the resolved round trip then matches, and JoinLocalized adds a
    // second separator -- a depfile rule target the build never asked for.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src\)", R"(C:\Users\RUNNER~1\p\build\)", resolver };

    auto const translated = reconciler.Path(R"(C:\Users\runneradmin\p\build\a.o)");
    CHECK(translated == R"(C:\Users\RUNNER~1\p\build\a.o)");
    CHECK_FALSE(translated.contains(R"(\\)"));
}

TEST_CASE("Region reconciles a depfile's dependencies and preserves the named output")
{
    // The rule target is the `-o` path the BUILD SYSTEM named; respelling it hands
    // back an output it never asked for, which Ninja rejects outright. Named by
    // value, because -MP's phony rules put a compiler-reported HEADER in target
    // position and that one must be reconciled like any other.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    constexpr std::string_view depFile = "C:\\Users\\runneradmin\\p\\build\\a.o: C:\\Users\\runneradmin\\p\\src\\a.cpp\\\n"
                                         "  C:\\Users\\runneradmin\\p\\src\\inc\\h1.h\n"
                                         "\n"
                                         "C:\\Users\\runneradmin\\p\\src\\inc\\h1.h:\n";

    std::vector<std::string> const targets { R"(C:\Users\runneradmin\p\build\a.o)" };
    auto const out = reconciler.Region(depFile, PathCanon::Grammar::GccDepfile, targets);
    CHECK(out
          == "C:\\Users\\runneradmin\\p\\build\\a.o: C:\\Users\\RUNNER~1\\p\\src\\a.cpp\\\n"
             "  C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h\n"
             "\n"
             "C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h:\n");
}

TEST_CASE("Region reconciles a showIncludes stream and leaves everything else alone")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    constexpr std::string_view region = "char const* s = \"Note: including file: x\";\n"
                                        "Note: including file: C:\\Users\\runneradmin\\p\\src\\inc\\h1.h\r\n";
    CHECK(reconciler.Region(region, PathCanon::Grammar::ShowIncludes)
          == "char const* s = \"Note: including file: x\";\n"
             "Note: including file: C:\\Users\\RUNNER~1\\p\\src\\inc\\h1.h\r\n");
}

TEST_CASE("IsInTree answers the question the key asks, relative paths included")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    CHECK(reconciler.IsInTree(R"(C:\Users\runneradmin\p\src\a.cpp)")); // only after reconciling
    CHECK(reconciler.IsInTree(R"(C:\Users\RUNNER~1\p\src\a.cpp)"));
    CHECK_FALSE(reconciler.IsInTree(R"(D:\elsewhere\a.cpp)"));

    // A relative path resolves against the compile's working directory, so it is
    // already machine-independent -- which is how KeyDependencySet and ReplayGuard
    // both treat one. A third answer here would make the launcher contradict
    // itself about one path, and would put the roots-mismatch note on every compile of
    // a build that passes relative sources.
    CHECK(reconciler.IsInTree("src/a.cpp"));
    CHECK_FALSE(reconciler.IsInTree(""));
}

TEST_CASE("All reconciles a whole dependency list in place")
{
    auto resolver = ShortNameHost();
    RootReconciler reconciler { R"(C:\Users\RUNNER~1\p\src)", R"(C:\Users\RUNNER~1\p\build)", resolver };

    std::vector<std::string> paths { R"(C:\Users\runneradmin\p\src\a.cpp)",
                                     R"(C:\Windows Kits\10\um\windows.h)",
                                     "inc/rel.hpp" };
    reconciler.All(paths);
    CHECK(paths[0] == R"(C:\Users\RUNNER~1\p\src\a.cpp)");
    CHECK(paths[1] == R"(C:\Windows Kits\10\um\windows.h)"); // outside both roots
    CHECK(paths[2] == "inc/rel.hpp");                        // relative, verbatim
}

TEST_CASE("An empty root is a prefix of nothing, not of everything")
{
    // RunCached refuses the cache outright without both roots, so this state does
    // not reach the launcher's flow -- but an empty string is a prefix of every
    // string, and a reconciler that took that literally would rewrite every path
    // it saw. PathCanon's IsSegmentPrefix is what makes it not, and this pins that
    // the reconciler inherits the answer rather than reimplementing it.
    auto resolver = ShortNameHost();
    RootReconciler reconciler { "", "", resolver };
    constexpr auto path = R"(C:\Users\runneradmin\p\src\a.cpp)";
    CHECK(reconciler.Path(path) == path);
    CHECK(reconciler.Region("Note: including file: C:\\x\\a.h\r\n", PathCanon::Grammar::ShowIncludes)
          == "Note: including file: C:\\x\\a.h\r\n");
}
