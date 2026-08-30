// SPDX-License-Identifier: Apache-2.0
#include "ParallelFor.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

using namespace FastCache;
using namespace FastCache::Cc;

// The interface's contract, asserted against BOTH implementations. They have to
// agree about a throwing slice, or a test that substitutes the serial one proves
// nothing about the threaded one that actually runs in production.

TEST_CASE("Every slice runs exactly once", "[parallelfor]")
{
    SerialParallelFor serial;
    ThreadedParallelFor threaded { 4 };
    IParallelFor* implementations[] = { &serial, &threaded };

    for (auto* parallel: implementations)
    {
        constexpr std::size_t Count = 200;
        std::vector<std::atomic<int>> seen(Count);
        REQUIRE(parallel->Run(Count, [&](std::size_t index) { seen[index].fetch_add(1); }));

        for (std::size_t index = 0; index < Count; ++index)
            CHECK(seen[index].load() == 1);
    }
}

TEST_CASE("A slice that throws is reported and does not stop the rest", "[parallelfor]")
{
    // The contract `ProbeToolchainFiles` depends on: the failure is not lost, and
    // the remaining slices still run so a partial result is partial rather than
    // unknown.
    SerialParallelFor serial;
    ThreadedParallelFor threaded { 4 };
    IParallelFor* implementations[] = { &serial, &threaded };

    for (auto* parallel: implementations)
    {
        constexpr std::size_t Count = 50;
        std::atomic<std::size_t> completed { 0 };
        auto const ok = parallel->Run(Count, [&](std::size_t index) {
            if (index == 7)
                throw std::runtime_error { "one slice fails" };
            completed.fetch_add(1);
        });

        CHECK_FALSE(ok);
        CHECK(completed.load() == Count - 1);
    }
}

TEST_CASE("Zero slices is legal and succeeds", "[parallelfor]")
{
    SerialParallelFor serial;
    ThreadedParallelFor threaded { 4 };
    CHECK(serial.Run(0, [](std::size_t) { FAIL("no slice may run"); }));
    CHECK(threaded.Run(0, [](std::size_t) { FAIL("no slice may run"); }));
}

TEST_CASE("The threaded width is bounded below by one and never exceeds the slice count", "[parallelfor]")
{
    // A width of zero would spawn nothing and hang the join loop on work never
    // taken; clamped, it degenerates to serial.
    ThreadedParallelFor none { 0 };
    CHECK(none.Width() == 1);

    std::atomic<int> ran { 0 };
    CHECK(none.Run(3, [&](std::size_t) { ran.fetch_add(1); }));
    CHECK(ran.load() == 3);

    // More width than work must not spawn a thread per unit of unused width.
    ThreadedParallelFor wide { 64 };
    std::mutex guard;
    std::set<std::size_t> indices;
    CHECK(wide.Run(2, [&](std::size_t index) {
        std::scoped_lock const lock { guard };
        indices.insert(index);
    }));
    CHECK(indices.size() == 2);
}

TEST_CASE("The default width oversubscribes deliberately and stays bounded", "[parallelfor]")
{
    // The number is derived rather than literal, so what is asserted is the shape
    // of the derivation: a floor so a single-core runner still overlaps waits, and
    // a ceiling so the cost does not run away on a large machine.
    auto const width = DefaultParallelWidth();
    CHECK(width >= 4);
    CHECK(width <= 32);
}
