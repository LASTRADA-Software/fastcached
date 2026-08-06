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
