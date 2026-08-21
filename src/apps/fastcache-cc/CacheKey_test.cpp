// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"
#include "KeyDigest.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
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
    KeyInputs const a { .compilerId = "cl", .preprocessed = "int f();", .relativizedArgs = dev, .dependencyPaths = {} };
    KeyInputs const b { .compilerId = "cl", .preprocessed = "int f();", .relativizedArgs = ci, .dependencyPaths = {} };
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
        .dependencyPaths = {},
    };
    KeyInputs const fromCi {
        .compilerId = "cl 19.51",
        .preprocessed = "int f(){return 1;}",
        .relativizedArgs = Relativize({ R"(D:\ci\deep\checkout\src\x.cpp)", R"(/ID:\ci\deep\checkout\src\inc)" },
                                      R"(D:\ci\deep\checkout\src)"),
        .dependencyPaths = {},
    };
    CHECK(ComputeKey(fromDev) == ComputeKey(fromCi));
}

TEST_CASE("ComputeKey differs when the preprocessed content differs")
{
    KeyInputs a { .compilerId = "cl", .preprocessed = "int f(){return 1;}", .relativizedArgs = {}, .dependencyPaths = {} };
    KeyInputs b { .compilerId = "cl", .preprocessed = "int f(){return 2;}", .relativizedArgs = {}, .dependencyPaths = {} };
    CHECK(ComputeKey(a) != ComputeKey(b));
}

TEST_CASE("ComputeKey differs when the compiler identity differs")
{
    KeyInputs a { .compilerId = "cl 19.51", .preprocessed = "x", .relativizedArgs = {}, .dependencyPaths = {} };
    KeyInputs b { .compilerId = "clang-cl 18", .preprocessed = "x", .relativizedArgs = {}, .dependencyPaths = {} };
    CHECK(ComputeKey(a) != ComputeKey(b));
}

TEST_CASE("ComputeKey is a stable 32-hex-char string")
{
    KeyInputs a { .compilerId = "cl", .preprocessed = "hello", .relativizedArgs = { "/c" }, .dependencyPaths = {} };
    auto const k = ComputeKey(a);
    CHECK(k.size() == KeyDigest::HexLength);
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
    KeyInputs const a {
        .compilerId = "g++ 14", .preprocessed = "int main(){}", .relativizedArgs = fromDeep, .dependencyPaths = {}
    };
    KeyInputs const b {
        .compilerId = "g++ 14", .preprocessed = "int main(){}", .relativizedArgs = fromShallow, .dependencyPaths = {}
    };
    CHECK(ComputeKey(a) == ComputeKey(b));
}

TEST_CASE("A different dependency path set is a different key")
{
    // The whole point of issue #56. A header that MOVES without changing a byte
    // leaves the preprocessed text and the arguments identical — line markers are
    // suppressed, so no path reaches either — and until the dependency set was
    // folded in, the key was identical too. The object was still correct and still
    // served; the depfile it came with named a file that no longer exists, and the
    // build never converged.
    KeyInputs before { .compilerId = "g++ 16",
                       .preprocessed = "inline int answer(){return 42;}",
                       .relativizedArgs = { "-c", "<SRCROOT>/t.cpp" },
                       .dependencyPaths = { "<SRCROOT>/inc/old/Hdr.hpp" } };
    KeyInputs after = before;
    after.dependencyPaths = { "<SRCROOT>/inc/new/Hdr.hpp" };

    CHECK(ComputeKey(before) != ComputeKey(after));
}

TEST_CASE("The dependency set is hashed as a list, not concatenated into the args")
{
    // The two lists are adjacent and both hold path-shaped strings, so without a
    // separator of its own a dependency could be read as a trailing argument and
    // two genuinely different compiles would collide.
    KeyInputs const asArg {
        .compilerId = "g++ 16", .preprocessed = "x", .relativizedArgs = { "-c", "<SRCROOT>/a.hpp" }, .dependencyPaths = {}
    };
    KeyInputs const asDependency {
        .compilerId = "g++ 16", .preprocessed = "x", .relativizedArgs = { "-c" }, .dependencyPaths = { "<SRCROOT>/a.hpp" }
    };

    CHECK(ComputeKey(asArg) != ComputeKey(asDependency));
}

TEST_CASE("An empty dependency set still keys stably")
{
    // A probe that reports nothing must not be fatal: the launcher keys with an
    // empty set rather than refusing to cache, so a driver we cannot get
    // dependencies out of still gets a working (if move-blind) cache.
    KeyInputs const inputs { .compilerId = "g++ 16",
                             .preprocessed = "int main(){}",
                             .relativizedArgs = { "-c", "<SRCROOT>/a.cpp" },
                             .dependencyPaths = {} };

    CHECK(ComputeKey(inputs) == ComputeKey(inputs));
    CHECK(ComputeKey(inputs).size() == KeyDigest::HexLength);
}

// --- issue #63: the key must actually be as wide as it looks -----------------
//
// The two cases below are the regression cover for a construction that produced
// a 32-hex-char key carrying 32 bits of strength. It took four CRC32C digests of
// one blob, distinguished only by a leading salt byte. CRC is affine over GF(2),
// so with `A` the per-byte state-update operator and `S_i` the state after salt
// `i`, quarter_i XOR quarter_j is `A^len(blob) * (S_i XOR S_j)` — a value that
// depends on the blob's LENGTH and on nothing else about it. Matching one quarter
// therefore forced all four, and a collision served an unrelated translation
// unit's object file under a zero exit code.

namespace
{
/// Split a 32-hex-char key into its four 32-bit quarters.
/// @param key A key as ComputeKey renders it.
/// @return The four quarters, most significant first.
std::array<std::uint32_t, 4> Quarters(std::string const& key)
{
    REQUIRE(key.size() == KeyDigest::HexLength);
    std::array<std::uint32_t, 4> out {};
    for (auto const index: std::views::iota(std::size_t { 0 }, out.size()))
        out[index] = static_cast<std::uint32_t>(std::stoul(key.substr(index * 8, 8), nullptr, 16));
    return out;
}

/// A deterministic byte source.
///
/// Hand-rolled rather than `std::mt19937` plus a distribution, because the
/// standard leaves a distribution's mapping from engine output to values
/// unspecified: "fixed seed" would not mean the same inputs on libstdc++, libc++
/// and MSVC's STL, and the sample count below is chosen against a specific
/// observed collision index. It has to be the same sequence everywhere.
class SplitMix64
{
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept: _state { seed } {}

    /// @return The next 64 pseudorandom bits.
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto z = _state;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    /// Produce a distinct 64-character text. The width is fixed on purpose: see
    /// the equal-length note in the cases below.
    /// @return 64 hex characters of fresh pseudorandom state.
    [[nodiscard]] std::string NextFixedWidthText()
    {
        std::string out;
        for ([[maybe_unused]] auto const word: std::views::iota(0, 4))
            out += std::format("{:016x}", Next());
        return out;
    }

  private:
    std::uint64_t _state;
};
} // namespace

TEST_CASE("The quarters of a key vary independently for equal-length inputs")
{
    // The direct expression of the defect, and deterministic: no seed luck, no
    // birthday search. Under the salted-CRC construction every one of the six
    // pairwise XORs took exactly ONE value across any number of equal-length
    // inputs.
    //
    // EQUAL LENGTH IS THE WHOLE POINT OF THIS CASE. The broken construction's
    // XOR does vary once the lengths differ — that is precisely what `A^len`
    // means — so relaxing these inputs to varying-length text would leave a test
    // that passes against the bug it exists to catch.
    constexpr std::size_t Samples = 64;

    SplitMix64 source { 63 };
    std::array<std::set<std::uint32_t>, 6> pairwiseXors;
    for ([[maybe_unused]] auto const sample: std::views::iota(std::size_t { 0 }, Samples))
    {
        KeyInputs const inputs { .compilerId = "cc-1.0",
                                 .preprocessed = source.NextFixedWidthText(),
                                 .relativizedArgs = {},
                                 .dependencyPaths = {} };
        auto const quarters = Quarters(ComputeKey(inputs));

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

TEST_CASE("Equal-length key inputs survive a birthday-sized collision search")
{
    // What the defect actually cost: a full 32-hex-char key collision between two
    // unrelated translation units, which is a wrong object served with a zero
    // exit code rather than a miss.
    //
    // The sample count is measured, not guessed. With this seed and this input
    // shape the salted-CRC construction produced its first full-key collision at
    // sample index 86,125 — the birthday bound for the 32 bits it really had —
    // so 100,000 makes the pre-fix failure deterministic rather than likely.
    // Against a real 128-bit digest this can never fire: the same bound sits at
    // ~2^64 samples.
    constexpr std::size_t Samples = 100'000;

    SplitMix64 source { 63 };
    std::unordered_set<std::string> keys;
    keys.reserve(Samples);
    for ([[maybe_unused]] auto const sample: std::views::iota(std::size_t { 0 }, Samples))
    {
        KeyInputs const inputs { .compilerId = "cc-1.0",
                                 .preprocessed = source.NextFixedWidthText(),
                                 .relativizedArgs = {},
                                 .dependencyPaths = {} };
        keys.insert(ComputeKey(inputs));
    }

    CHECK(keys.size() == Samples);
}

TEST_CASE("ComputeKey's value is pinned, so changing the construction is deliberate")
{
    // A pin, not a correctness proof. Correctness of the digest underneath is
    // what MurmurHash3_test.cpp's SMHasher verification value covers; this vector
    // was read out of the implementation, so all it can prove is that the value
    // has not moved since a human reviewed the construction.
    //
    // That is exactly the job. Every input to this key -- the schema tag, the
    // field order, the separator bytes, the digest, its rendering -- is a thing
    // whose change re-keys every cached object on every machine sharing the
    // cache, and none of them announces itself.
    //
    // IF THIS FAILS, the key construction changed. That is a schema change:
    //   1. bump `objkey-v*` in CacheKey.cpp AND `manifest-v*` in
    //      DirectManifest.cpp, in lock-step -- a manifest stores the object key
    //      by value and its own key never sees the object-key schema, so direct
    //      mode would keep resolving to entries written under the old rules;
    //   2. only then update this vector.
    // Updating the vector alone leaves old entries matching new keys and being
    // served under rules they were not written by, which is the silent mis-serve
    // the tag exists to prevent -- it presents as a hit-rate collapse, not a miss.
    KeyInputs const inputs { .compilerId = "g++ (GCC) 16.0.0",
                             .preprocessed = "int main() { return 0; }\n",
                             .relativizedArgs = { "-c", "-O2", "<SRCROOT>/src/main.cpp" },
                             .dependencyPaths = { "<SRCROOT>/inc/a.hpp", "<SRCROOT>/inc/b.hpp" } };

    CHECK(ComputeKey(inputs) == "c555e22cc49a05fabcfbeb6986e85069");
}
