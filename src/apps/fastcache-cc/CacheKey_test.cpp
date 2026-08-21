// SPDX-License-Identifier: Apache-2.0
#include "CacheKey.hpp"
#include "KeyDigest.hpp"
#include "KeyDigestTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

using namespace FastCache::Cc;
using FastCache::Cc::Test::DigestQuarters;
using FastCache::Cc::Test::SplitMix64;

namespace
{
std::vector<std::string> Relativize(std::vector<std::string> const& args,
                                    std::string_view root,
                                    std::string_view buildTree = {})
{
    return RelativizeArgs(std::span<std::string const> { args }, root, buildTree);
}
} // namespace

TEST_CASE("RelativizeArgs reconciles an argument's spelling before testing the roots")
{
    // The argument half of issue #66. A generator can spell an include directory
    // in a form nothing else in the build uses — an 8.3 short component, a `subst`
    // drive — and the launcher's roots are resolved, so without reconciliation the
    // path lies under neither and the checkout location stays in the key.
    auto const dealias = [](std::string_view path) -> std::string {
        constexpr std::string_view Aliased = R"(C:\Users\RUNNER~1\src)";
        constexpr std::string_view Real = R"(C:\Users\runneradmin\src)";
        if (!path.starts_with(Aliased))
            return std::string { path };
        return std::string { Real } + std::string { path.substr(Aliased.size()) };
    };
    std::vector<std::string> const args { R"(/IC:\Users\RUNNER~1\src\inc)", R"(C:\Users\RUNNER~1\src\x.cpp)" };
    std::string_view const root = R"(C:\Users\runneradmin\src)";

    // Without it, both come back exactly as written and neither is portable.
    auto const raw = RelativizeArgs(std::span<std::string const> { args }, root, {});
    CHECK(raw[0] == args[0]);
    CHECK(raw[1] == args[1]);

    auto const out = RelativizeArgs(std::span<std::string const> { args }, root, {}, dealias);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "/I<SRCROOT>/inc");
    CHECK(out[1] == "<SRCROOT>/x.cpp");
}

TEST_CASE("RelativizeArgs keeps an argument's own spelling when reconciling did not help")
{
    // Reconciliation exists to make the ROOT TEST succeed. An argument it did not
    // place under a root must come back as WRITTEN, not in its reconciled form —
    // otherwise a path the cache has no opinion about changes the key.
    auto const rewriteEverything = [](std::string_view) -> std::string {
        return R"(D:\somewhere\else\x.cpp)";
    };
    std::vector<std::string> const args { R"(C:\elsewhere\x.cpp)", "-DFOO=bar" };
    auto const out = RelativizeArgs(std::span<std::string const> { args }, R"(C:\a\b\src)", {}, rewriteEverything);
    REQUIRE(out.size() == 2);
    CHECK(out[0] == args[0]);
    CHECK(out[1] == "-DFOO=bar"); // an option never reaches the transform at all
}

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
        KeyInputs const inputs {
            .compilerId = "cc-1.0", .preprocessed = source.NextFixedWidthText(), .relativizedArgs = {}, .dependencyPaths = {}
        };
        auto const quarters = DigestQuarters(ComputeKey(inputs));

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
        KeyInputs const inputs {
            .compilerId = "cc-1.0", .preprocessed = source.NextFixedWidthText(), .relativizedArgs = {}, .dependencyPaths = {}
        };
        keys.insert(ComputeKey(inputs));
    }

    CHECK(keys.size() == Samples);
}

TEST_CASE("ComputeKey's value is pinned, so changing the construction is deliberate")
{
    // A pin. Correctness of the digest underneath is what MurmurHash3_test.cpp's
    // SMHasher verification value covers; what this adds is that the value has
    // not moved since the construction was reviewed.
    //
    // It is a little stronger than the usual golden, which can only ever say
    // "unchanged since someone pasted it": this vector was independently
    // reproduced from the MurmurHash3 specification and the grammar below --
    // kind byte, big-endian u64 length, bytes, per piece -- by a separate
    // implementation. So it pins the whole chain, tag placement and field order
    // and separator kinds and rendering included, not just the digest.
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

    CHECK(ComputeKey(inputs) == "a38a64d1e6e4c72f555c7e97ba26bd16");
}

TEST_CASE("Field contents cannot be shifted across a field boundary")
{
    // The framing had to be length-prefixed rather than separator-terminated,
    // and this is the case that forces it. Terminating a value with a byte that
    // can occur INSIDE a value is not a framing at all: while `Field` wrote
    // `value` followed by NUL, these two inputs produced byte-identical blobs
    // and therefore the same key.
    //
    // That is the same silent mis-serve as issue #63 -- two unrelated
    // translation units sharing a cache entry, served with a zero exit code --
    // reached by a different route, and it was reachable rather than
    // theoretical: preprocessed text can carry a raw NUL, and a build system can
    // pass an argument containing 0x01.
    //
    // It is fixed here rather than filed because v3 re-keys the whole cache
    // anyway, so the invalidation is already paid; discovering it later would
    // have cost a v4 for a defect of the class this very change exists to close.
    KeyInputs const shifted {
        .compilerId = std::string { "cc\0d", 4 }, .preprocessed = "x", .relativizedArgs = {}, .dependencyPaths = {}
    };
    KeyInputs const other {
        .compilerId = "cc", .preprocessed = std::string { "d\0x", 3 }, .relativizedArgs = {}, .dependencyPaths = {}
    };

    CHECK(ComputeKey(shifted) != ComputeKey(other));

    // The same shift across the argument and dependency lists, whose separators
    // were 0x01 and 0x02.
    KeyInputs const argSplit {
        .compilerId = "cc", .preprocessed = "x", .relativizedArgs = { std::string { "-a\x01-b", 5 } }, .dependencyPaths = {}
    };
    KeyInputs const argWhole {
        .compilerId = "cc", .preprocessed = "x", .relativizedArgs = { "-a", "-b" }, .dependencyPaths = {}
    };
    CHECK(ComputeKey(argSplit) != ComputeKey(argWhole));

    // And a value must not digest the same when it is fed as a different KIND of
    // piece: an argument and a dependency path are adjacent lists of
    // path-shaped strings.
    KeyInputs const asArg {
        .compilerId = "cc", .preprocessed = "x", .relativizedArgs = { "<SRCROOT>/a.hpp" }, .dependencyPaths = {}
    };
    KeyInputs const asPath {
        .compilerId = "cc", .preprocessed = "x", .relativizedArgs = {}, .dependencyPaths = { "<SRCROOT>/a.hpp" }
    };
    CHECK(ComputeKey(asArg) != ComputeKey(asPath));
}

// --- the object output must be relativized in EITHER spelling ----------------

TEST_CASE("RelativizeArgs tokenizes a fused object-output path on both drivers")
{
    // Regression guard. The separated spellings (`/Fo <path>`, `-o <path>`) reach
    // the bare-path branch, because the value is then an argument of its own — so
    // they were relativized, and the case above ("a leading slash still introduces
    // an option") passed while the fused form was broken. The fused ones went
    // through the flag table, which listed the include-dir prefixes and nothing
    // else, so `/Fo<abs>` came back verbatim with the producing machine's object
    // path still in it.
    //
    // Every build system that drives MSVC writes the fused form, so on Windows
    // this was not an edge case: it was the shape of every compile, and two
    // checkouts at different roots could never share an entry.
    auto const msvc = Relativize({ R"(/FoD:\s\deep\a\b\build\u.obj)" }, R"(D:\s\deep\a\b\src)", R"(D:\s\deep\a\b\build)");
    CHECK(msvc[0] == "/Fo<BUILDTREE>/u.obj");

    auto const gnu = Relativize({ "-o/home/dev/proj/build/u.o" }, "/home/dev/proj", "/home/dev/proj/build");
    CHECK(gnu[0] == "-o<BUILDTREE>/u.o");
}

TEST_CASE("The fused and separated object-output spellings relativize to the same value")
{
    // The property the two tables could not keep: which spelling the build system
    // chose is not a fact about the compilation, so it must not change what the
    // key sees of the object path.
    auto const fused = Relativize({ R"(/FoC:\src\proj\build\a.obj)" }, R"(C:\src\proj)", R"(C:\src\proj\build)");
    auto const separated = Relativize({ "/Fo", R"(C:\src\proj\build\a.obj)" }, R"(C:\src\proj)", R"(C:\src\proj\build)");

    REQUIRE(separated.size() == 2);
    CHECK(fused[0] == "/Fo" + separated[1]);
}

TEST_CASE("A fused object output does not keep two Windows checkouts from sharing a key")
{
    // What the defect actually cost, stated as the property the launcher exists
    // to provide. This is the MSVC mirror of "Two POSIX checkouts at different
    // depths relativize identically", and it failed on every Windows build:
    // `/Fo` carries an absolute path under the build tree, so the deep and the
    // shallow checkout hashed different argument lists for identical content.
    std::vector<std::string> const deep { "/nologo",
                                          "/c",
                                          "/showIncludes",
                                          R"(/FoD:\s\deep\a\b\build\u.obj)",
                                          R"(/ID:\s\deep\a\b\src\inc)",
                                          R"(D:\s\deep\a\b\src\u.cpp)" };
    std::vector<std::string> const shallow { "/nologo",           "/c",
                                             "/showIncludes",     R"(/FoC:\w\build\u.obj)",
                                             R"(/IC:\w\src\inc)", R"(C:\w\src\u.cpp)" };

    auto const fromDeep = RelativizeArgs(deep, R"(D:\s\deep\a\b\src)", R"(D:\s\deep\a\b\build)");
    auto const fromShallow = RelativizeArgs(shallow, R"(C:\w\src)", R"(C:\w\build)");
    CHECK(fromDeep == fromShallow);

    KeyInputs const a {
        .compilerId = "cl 19.51", .preprocessed = "int g(){return 1;}", .relativizedArgs = fromDeep, .dependencyPaths = {}
    };
    KeyInputs const b {
        .compilerId = "cl 19.51", .preprocessed = "int g(){return 1;}", .relativizedArgs = fromShallow, .dependencyPaths = {}
    };
    CHECK(ComputeKey(a) == ComputeKey(b));
}

TEST_CASE("A fused value joined through a separator keeps its separator")
{
    // `-MF=dep.d` is the same flag as `-MFdep.d`; the separator belongs to the
    // flag, not to the path, so it must survive the rewrite rather than be
    // canonicalized as part of the value or dropped from the argument.
    auto const out = Relativize({ "-MF=/home/dev/proj/build/a.d" }, "/home/dev/proj", "/home/dev/proj/build");
    CHECK(out[0] == "-MF=<BUILDTREE>/a.d");
}

TEST_CASE("A relative object output is left exactly as the build system spelled it")
{
    // The complement, and the reason this change re-keys only the builds it has
    // to: a path under neither root canonicalizes to itself, so the argument
    // comes back byte-for-byte and its key does not move.
    auto const msvc = Relativize({ R"(/Fobuild\u.obj)" }, R"(C:\src\proj)", R"(C:\src\proj\build)");
    CHECK(msvc[0] == R"(/Fobuild\u.obj)");

    auto const gnu = Relativize({ "-obuild/u.o" }, "/home/dev/proj", "/home/dev/proj/build");
    CHECK(gnu[0] == "-obuild/u.o");
}
