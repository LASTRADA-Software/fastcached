// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"

#include <catch2/catch_test_macros.hpp>

#include <span>
#include <string>
#include <vector>

using namespace FastCache::Cc;

namespace
{
std::vector<std::string> Relativize(std::vector<std::string> const& args,
                                    std::string_view root,
                                    std::string_view buildTree = {})
{
    return RelativizeArgs(std::span<std::string const> { args }, root, buildTree);
}
} // namespace

TEST_CASE("RelativizeArgs tokenizes a source path under the source root")
{
    auto const out = Relativize({ R"(C:\a\b\src\x.cpp)" }, R"(C:\a\b\src)");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "<SRCROOT>/x.cpp");
}

TEST_CASE("RelativizeArgs tokenizes fused and separate include-dir args")
{
    auto const fused = Relativize({ R"(/IC:\a\b\src\inc)" }, R"(C:\a\b\src)");
    CHECK(fused[0] == "/I<SRCROOT>/inc");

    auto const external = Relativize({ R"(/external:IC:\a\b\src\vendor)" }, R"(C:\a\b\src)");
    CHECK(external[0] == "/external:I<SRCROOT>/vendor");
}

TEST_CASE("RelativizeArgs tokenizes a build-tree include nested under the source root")
{
    // The build tree lives inside the checkout (…\out\build). An include into
    // it must become <BUILDTREE>/… — NOT <SRCROOT>/out/build/… — so the key is
    // stable across checkouts at different roots. This was a real cross-folder
    // caching bug: relativizing only the source root left these absolute.
    auto const out = Relativize(
        { R"(/external:IC:\co\src\out\build\vcpkg_installed\x64\include)" }, R"(C:\co\src)", R"(C:\co\src\out\build)");
    REQUIRE(out.size() == 1);
    CHECK(out[0] == "/external:I<BUILDTREE>/vcpkg_installed/x64/include");
}

TEST_CASE("ComputeKey matches across roots when build-tree includes are present (the cross-folder case)")
{
    auto const dev =
        Relativize({ R"(C:\dev\src\x.cpp)", R"(/external:IC:\dev\src\out\build\vcpkg\include)", R"(/IC:\dev\src\inc)" },
                   R"(C:\dev\src)",
                   R"(C:\dev\src\out\build)");
    auto const ci = Relativize(
        { R"(D:\ci\deep\src\x.cpp)", R"(/external:ID:\ci\deep\src\out\build\vcpkg\include)", R"(/ID:\ci\deep\src\inc)" },
        R"(D:\ci\deep\src)",
        R"(D:\ci\deep\src\out\build)");
    KeyInputs const a { .compilerId = "cl", .preprocessed = "int f();", .relativizedArgs = dev };
    KeyInputs const b { .compilerId = "cl", .preprocessed = "int f();", .relativizedArgs = ci };
    CHECK(ComputeKey(a) == ComputeKey(b));
}

TEST_CASE("RelativizeArgs leaves args outside the source root untouched")
{
    auto const out = Relativize({ "/O2", R"(/IC:\Windows\SDK)" }, R"(C:\a\b\src)");
    CHECK(out[0] == "/O2");
    CHECK(out[1] == R"(/IC:\Windows\SDK)");
}

TEST_CASE("ComputeKey is identical for the same content relativized from different roots")
{
    KeyInputs const fromDev {
        .compilerId = "cl 19.51",
        .preprocessed = "int f(){return 1;}",
        .relativizedArgs = Relativize({ R"(C:\dev\src\x.cpp)", R"(/IC:\dev\src\inc)" }, R"(C:\dev\src)"),
    };
    KeyInputs const fromCi {
        .compilerId = "cl 19.51",
        .preprocessed = "int f(){return 1;}",
        .relativizedArgs = Relativize({ R"(D:\ci\deep\checkout\src\x.cpp)", R"(/ID:\ci\deep\checkout\src\inc)" },
                                      R"(D:\ci\deep\checkout\src)"),
    };
    CHECK(ComputeKey(fromDev) == ComputeKey(fromCi));
}

TEST_CASE("ComputeKey differs when the preprocessed content differs")
{
    KeyInputs a { .compilerId = "cl", .preprocessed = "int f(){return 1;}", .relativizedArgs = {} };
    KeyInputs b { .compilerId = "cl", .preprocessed = "int f(){return 2;}", .relativizedArgs = {} };
    CHECK(ComputeKey(a) != ComputeKey(b));
}

TEST_CASE("ComputeKey differs when the compiler identity differs")
{
    KeyInputs a { .compilerId = "cl 19.51", .preprocessed = "x", .relativizedArgs = {} };
    KeyInputs b { .compilerId = "clang-cl 18", .preprocessed = "x", .relativizedArgs = {} };
    CHECK(ComputeKey(a) != ComputeKey(b));
}

TEST_CASE("ComputeKey is a stable 32-hex-char string")
{
    KeyInputs a { .compilerId = "cl", .preprocessed = "hello", .relativizedArgs = { "/c" } };
    auto const k = ComputeKey(a);
    CHECK(k.size() == 32);
    CHECK(k == ComputeKey(a)); // deterministic
}

TEST_CASE("RelativizeArgs tokenizes POSIX absolute paths under either root")
{
    // Regression guard. A leading '/' introduces an option only on Windows; on
    // POSIX it starts an absolute path. Skipping those left the checkout path
    // in the cache key, so two checkouts of identical content at different
    // paths keyed differently and never shared a hit — the exact property this
    // launcher exists to provide.
    std::vector<std::string> const args { "-c", "/home/dev/proj/a.cpp", "-o", "/home/dev/proj/build/a.o" };
    auto const out = RelativizeArgs(args, "/home/dev/proj", "/home/dev/proj/build");

    CHECK(out[1] != "/home/dev/proj/a.cpp"); // rewritten, not passed through
    CHECK_FALSE(out[1].contains("/home/dev"));
    CHECK_FALSE(out[3].contains("/home/dev"));
}

TEST_CASE("Under a Windows layout a leading slash still introduces an option")
{
    // The mirror of the case above, and the reason the decision is made from
    // the layout rather than from the host: MSVC options start with '/', so
    // under a Windows layout they must NOT be mistaken for absolute paths and
    // rewritten. Both directions have to hold on every platform, otherwise the
    // behaviour is only testable on the OS that happens to match.
    std::vector<std::string> const args { "/c", R"(C:\src\proj\a.cpp)", "/Fo", R"(C:\src\proj\build\a.obj)" };
    auto const out = RelativizeArgs(args, R"(C:\src\proj)", R"(C:\src\proj\build)");

    CHECK(out[0] == "/c");    // an option, left alone
    CHECK(out[2] == "/Fo");   // ditto
    CHECK(out[1] != args[1]); // the paths are still tokenized
    CHECK_FALSE(out[1].contains("C:"));
    CHECK_FALSE(out[3].contains("C:"));
}

TEST_CASE("A POSIX checkout whose root starts with /I is not mis-split as an include flag")
{
    // Regression guard. The include-prefix table is scanned before the bare-path
    // branch, so under a POSIX layout `/Infra/proj/a.cpp` used to match the MSVC
    // spelling `/I` and be split into the prefix `/I` plus `nfra/proj/a.cpp` —
    // a fragment under neither root, returned verbatim with the absolute
    // checkout path still in the key. Any root whose first segment begins with
    // a capital I (/Import, /Include) hits this.
    std::vector<std::string> const args { "-c", "/Infra/proj/a.cpp", "-o", "/Infra/proj/build/a.o" };
    auto const out = RelativizeArgs(args, "/Infra/proj", "/Infra/proj/build");

    CHECK(out[1] == "<SRCROOT>/a.cpp");
    CHECK(out[3] == "<BUILDTREE>/a.o");

    // ...and therefore two such checkouts at different roots share a key.
    std::vector<std::string> const other { "-c", "/Infra2/proj/a.cpp", "-o", "/Infra2/proj/build/a.o" };
    CHECK(RelativizeArgs(other, "/Infra2/proj", "/Infra2/proj/build") == out);
}

TEST_CASE("A root whose second byte is a colon is not mistaken for a Windows drive")
{
    // The drive-letter test must require a LETTER before the colon. Keying only
    // off `root[1] == ':'` classified a root like `a:b` as Windows, which turned
    // every leading '/' into an "option" and left absolute paths — and so the
    // checkout location — in the cache key. Only a root whose colon sits at
    // index 1 hits this; a colon deeper in the path was always safe.
    std::vector<std::string> const args { "-c", "/a:b/proj/a.cpp" };
    auto const out = Relativize(args, "a:b", "/a:b/proj");

    // The source path lies under neither root here, but it must still be
    // considered a path rather than skipped as an option.
    CHECK(out[1] == "<BUILDTREE>/a.cpp");
}

TEST_CASE("A drive-letter root spelled with forward slashes is still a Windows layout")
{
    // Pins existing behaviour rather than fixing a bug: this already held, and
    // the point is that it must keep holding now that the predicate moved into
    // PathCanon. CMake hands out `C:/src/proj`, which has no backslash, so a
    // separator-only test would call it POSIX and start rewriting MSVC options
    // as if they were absolute paths.
    std::vector<std::string> const args { "/c", "C:/src/proj/a.cpp" };
    auto const out = Relativize(args, "C:/src/proj");

    CHECK(out[0] == "/c"); // an option, not a path
    CHECK(out[1] == "<SRCROOT>/a.cpp");
}

TEST_CASE("Two POSIX checkouts at different depths relativize identically")
{
    std::vector<std::string> const deep { "-c", "/ci/w/1/s/proj/a.cpp", "-o", "/ci/w/1/s/proj/build/a.o" };
    std::vector<std::string> const shallow { "-c", "/home/a/proj/a.cpp", "-o", "/home/a/proj/build/a.o" };

    auto const fromDeep = RelativizeArgs(deep, "/ci/w/1/s/proj", "/ci/w/1/s/proj/build");
    auto const fromShallow = RelativizeArgs(shallow, "/home/a/proj", "/home/a/proj/build");
    CHECK(fromDeep == fromShallow);

    // ...and therefore produce the same key for identical content.
    KeyInputs const a { .compilerId = "g++ 14", .preprocessed = "int main(){}", .relativizedArgs = fromDeep };
    KeyInputs const b { .compilerId = "g++ 14", .preprocessed = "int main(){}", .relativizedArgs = fromShallow };
    CHECK(ComputeKey(a) == ComputeKey(b));
}
