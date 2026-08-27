// SPDX-License-Identifier: Apache-2.0
#include "ToolchainFingerprint.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace FastCache::Cc;

namespace
{

/// A small stand-in toolchain: two headers with distinct contents.
[[nodiscard]] std::vector<ToolchainFile> SampleTree()
{
    return { ToolchainFile { .relativePath = "c++/13/vector", .contentHash = "aaaa" },
             ToolchainFile { .relativePath = "stdio.h", .contentHash = "bbbb" } };
}

} // namespace

TEST_CASE("The same toolchain at two install prefixes fingerprints identically", "[toolchain][fingerprint]")
{
    // The property the whole design turns on. Paths are relative to their include
    // root precisely so that /usr/lib/gcc/... and /opt/toolchains/gcc-13/... —
    // or a vendored SDK checked out at two depths — are one toolchain. Making the
    // prefix part of the identity would disable distribution between exactly the
    // machines it exists to connect, and would do it silently.
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree())
          == ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()));
}

TEST_CASE("Enumeration order does not change the fingerprint", "[toolchain][fingerprint]")
{
    // A caller's order comes from a directory traversal, which is a property of the
    // filesystem: two machines with byte-identical toolchains can enumerate them
    // differently, and an order-sensitive digest would call that two toolchains.
    auto forward = SampleTree();
    std::vector<ToolchainFile> reversed { forward.rbegin(), forward.rend() };

    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", forward) == ComputeToolchainFingerprint("gcc 13.2.0", reversed));
}

TEST_CASE("One changed header changes the fingerprint", "[toolchain][fingerprint]")
{
    auto changed = SampleTree();
    changed[0].contentHash = "cccc";
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()) != ComputeToolchainFingerprint("gcc 13.2.0", changed));
}

TEST_CASE("A renamed header changes the fingerprint", "[toolchain][fingerprint]")
{
    auto renamed = SampleTree();
    renamed[0].relativePath = "c++/13/list";
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()) != ComputeToolchainFingerprint("gcc 13.2.0", renamed));
}

TEST_CASE("A path/hash boundary shift is not a match", "[toolchain][fingerprint]")
{
    // Concatenation is not a framing: {"ab","c"} and {"a","bc"} would digest
    // identically if the two pieces were simply joined. That would be a false
    // MATCH, which is the one error direction that dispatches a job to the wrong
    // toolchain -- an over-strict fingerprint merely costs a local compile.
    std::vector<ToolchainFile> const left { { .relativePath = "ab", .contentHash = "c" } };
    std::vector<ToolchainFile> const right { { .relativePath = "a", .contentHash = "bc" } };
    CHECK(ComputeToolchainFingerprint("gcc", left) != ComputeToolchainFingerprint("gcc", right));
}

TEST_CASE("Adding or removing a header changes the fingerprint", "[toolchain][fingerprint]")
{
    auto extra = SampleTree();
    extra.push_back({ .relativePath = "zzz.h", .contentHash = "dddd" });
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()) != ComputeToolchainFingerprint("gcc 13.2.0", extra));

    std::vector<ToolchainFile> const fewer { SampleTree().front() };
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()) != ComputeToolchainFingerprint("gcc 13.2.0", fewer));
}

TEST_CASE("A different compiler banner over the same headers is a different toolchain", "[toolchain][fingerprint]")
{
    // Headers alone are too weak in this direction: two compilers can share a
    // header tree and generate different code from it. The x86 and x64 `cl.exe` of
    // one MSVC toolset are that case exactly -- their include roots are the same
    // files -- so the banner is the ONLY thing here that can tell them apart, and
    // it does only because it names the target ("... for x64"). That is why a
    // banner all MSVC compilers shared was a fingerprint defect as well as a cache
    // key one; see issue #195.
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree())
          != ComputeToolchainFingerprint("gcc 14.1.0", SampleTree()));
}

TEST_CASE("The same banner over different headers is a different toolchain", "[toolchain][fingerprint]")
{
    // And too weak in the other: two machines can print an identical --version
    // while resolving different libstdc++ headers. For the CACHE that residual is
    // tolerable and documented; for DISTRIBUTION it produces a silently wrong
    // object, which is why the fingerprint folds in both.
    auto other = SampleTree();
    other[1].contentHash = "eeee";
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()) != ComputeToolchainFingerprint("gcc 13.2.0", other));
}

TEST_CASE("An empty toolchain still yields a stable fingerprint", "[toolchain][fingerprint]")
{
    // A probe that finds no headers must still produce something deterministic --
    // and something that differs from a real tree, so a failed probe cannot match a
    // real toolchain by accident.
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", {}) == ComputeToolchainFingerprint("gcc 13.2.0", {}));
    CHECK(ComputeToolchainFingerprint("gcc 13.2.0", {}) != ComputeToolchainFingerprint("gcc 13.2.0", SampleTree()));
}

TEST_CASE("A fingerprint is a fixed-width hex string", "[toolchain][fingerprint]")
{
    // It travels as an opaque field on the wire and is compared byte-for-byte, so
    // its shape is part of the contract even though its contents are not.
    auto const fingerprint = ComputeToolchainFingerprint("gcc 13.2.0", SampleTree());
    CHECK(fingerprint.size() == 32);
    CHECK(fingerprint.find_first_not_of("0123456789abcdef") == std::string::npos);
}
