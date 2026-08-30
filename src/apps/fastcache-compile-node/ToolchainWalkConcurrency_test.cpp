// SPDX-License-Identifier: Apache-2.0
//
// The toolchain walk, driven through the REAL threaded implementation.
//
// Why this file is here rather than beside `ToolchainProbe.cpp`: TSan coverage.
// `scripts/tsan-gate.sh` builds two binaries, and its row for this one is
//
//     "fastcache-compile-node-tests|"
//
// -- an EMPTY tag expression, which that script documents as "run the whole
// binary". So every case in this target is sanitized, with no tag needed and
// without widening the TSan build scope. `fastcache-cc-tests` is not a target the
// gate builds at all, so the same cases beside `ToolchainProbe_test.cpp` would be
// unsanitized while looking exactly as covered.
//
// `ToolchainProbe.cpp` is compiled into this target through `_fc_node_shared`
// (`CMakeLists.txt`), and the node is also the process that actually pays the
// cold walk in production -- so the coverage lands on the binary that runs it.
//
// The cases beside `ToolchainProbe.cpp` assert what the walk COVERS, serially and
// deterministically. These assert that running it concurrently changes nothing and
// races nothing.
#include "NodeToolchains.hpp"

#include <ParallelFor.hpp>
#include <ToolchainProbe.hpp>

#include <catch2/catch_test_macros.hpp>

#include <tests/ScratchPath.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
#include <string>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{
    /// A root with `files` distinct files, each with distinct contents.
    ///
    /// Distinct contents on purpose: identical files would hash identically, so a
    /// slice that wrote another slice's result would be invisible.
    void PopulateRoot(std::filesystem::path const& root, std::size_t files)
    {
        std::filesystem::create_directories(root);
        for (std::size_t index = 0; index < files; ++index)
        {
            // Nested a little, so the walk recurses rather than reading one flat
            // directory -- the relative spelling is part of the digest.
            auto const dir = root / ("d" + std::to_string(index % 7));
            std::filesystem::create_directories(dir);
            std::ofstream out { dir / ("h" + std::to_string(index) + ".hpp"), std::ios::binary };
            out << "// header " << index << '\n' << std::string(index % 64, 'x') << '\n';
        }
    }
}

TEST_CASE("A concurrent walk yields the same fingerprint as a serial one", "[toolchain][concurrency]")
{
    // The property that makes parallelising this safe at all: `ToolchainFileScan`
    // is documented "unsorted (the digest sorts)", so the value cannot depend on
    // the order slices finish in. If it ever does, this is where it shows.
    Testing::ScratchDirectory scratch { "fc-walk-concurrency" };
    auto const root = scratch.Path() / "include";
    PopulateRoot(root, 240);

    std::vector<std::string> const roots { root.string() };

    SerialParallelFor serial;
    auto const serialScan = ProbeToolchainFiles(roots, serial);

    ThreadedParallelFor threaded { 8 };
    auto const threadedScan = ProbeToolchainFiles(roots, threaded);

    CHECK(serialScan.complete);
    CHECK(threadedScan.complete);
    CHECK(serialScan.files.size() == threadedScan.files.size());
    CHECK(serialScan.files.size() == 240);

    CHECK(ComputeToolchainFingerprint("cc 1.0", serialScan.files)
          == ComputeToolchainFingerprint("cc 1.0", threadedScan.files));
}

TEST_CASE("Repeating a concurrent walk is stable", "[toolchain][concurrency]")
{
    // Run twice through the real threads. A race that only sometimes corrupts a
    // result would not reliably fail an equality check, which is exactly why this
    // case exists inside the binary ThreadSanitizer runs rather than as a
    // statistical assertion here.
    Testing::ScratchDirectory scratch { "fc-walk-concurrency-stable" };
    auto const root = scratch.Path() / "include";
    PopulateRoot(root, 180);

    std::vector<std::string> const roots { root.string() };

    ThreadedParallelFor threaded { 8 };
    auto const first = ProbeToolchainFiles(roots, threaded);
    auto const second = ProbeToolchainFiles(roots, threaded);

    REQUIRE(first.complete);
    REQUIRE(second.complete);
    CHECK(ComputeToolchainFingerprint("cc 1.0", first.files)
          == ComputeToolchainFingerprint("cc 1.0", second.files));
}

TEST_CASE("A slice that does not finish clears complete", "[toolchain][concurrency]")
{
    // THE correctness requirement of this change, and the one that is silent when
    // it breaks. `complete` is what stops a short walk being written to the cache:
    // `ComputeToolchainStamp` folds each root's path and mtime and never its
    // contents, so a truncated digest stored under a valid stamp validates forever
    // and no later run ever walks again to notice.
    //
    // Asserted through a fake rather than by arranging a real failure, because a
    // real one is a race and this must fail deterministically.
    class FailsOneSlice final: public IParallelFor
    {
      public:
        [[nodiscard]] bool Run(std::size_t count, std::function<void(std::size_t)> const& slice) override
        {
            auto ok = true;
            for (std::size_t index = 0; index < count; ++index)
            {
                if (index == count / 2)
                {
                    // Exactly what a throwing slice looks like to `Run`: it did not
                    // finish, and every other slice still ran.
                    ok = false;
                    continue;
                }
                slice(index);
            }
            return ok;
        }
    };

    Testing::ScratchDirectory scratch { "fc-walk-incomplete" };
    auto const root = scratch.Path() / "include";
    PopulateRoot(root, 20);

    std::vector<std::string> const roots { root.string() };

    FailsOneSlice failing;
    auto const scan = ProbeToolchainFiles(roots, failing);

    // The walk found the root and read most of it -- so nothing else in the result
    // says anything is wrong, which is the whole danger.
    CHECK_FALSE(scan.files.empty());
    CHECK_FALSE(scan.complete);
}
