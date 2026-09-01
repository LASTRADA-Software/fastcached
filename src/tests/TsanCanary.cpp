// SPDX-License-Identifier: Apache-2.0
//
// A deliberate data race, whose ONLY job is to be caught.
//
// ## Why this file contains a bug on purpose
//
// A sanitizer that is enabled in the cache but absent from the build is a tool
// that silently does nothing, and its output is indistinguishable from success.
// This is not hypothetical here: `cmake/portable/Sanitizers.cmake` carries the
// scar of exactly that defect, where CMP0126 left a normal variable shadowing
// the cache, `add_compile_options` received no `-fsanitize=` at all, and the
// configure log still printed "Enabling" while the cache still read ON. Every
// signal an author would check said yes and nothing was instrumented -- on this
// repository, where it meant the sanitizer presets had never once run a
// sanitizer.
//
// A green ThreadSanitizer job proves nothing unless something proves the
// sanitizer would have gone red. That is this file. It is built by the same
// CMake machinery as everything else, so it inherits its flags from the same
// `add_compile_options` that instruments the real targets -- which is what makes
// it a test of the MECHANISM rather than of a flag somebody retyped into a
// script. `scripts/tsan-gate.sh` refuses to believe a clean suite until this
// program has reported a race and exited non-zero.
//
// It is deliberately NOT linked against `FastCache`. It answers "is the
// toolchain instrumenting and reporting", which must stay answerable even when
// the library does not build.
//
// ## Why the race is spread across an array (#473)
//
// The previous version raced two threads on ONE `int`, a thousand increments
// each. It was silent -- the race happened and ThreadSanitizer did not report it
// -- in a few runs per thousand, and that is not a tolerable property for this
// program. A gate that is red a few percent of the time teaches everyone to
// re-run it, and a gate people reflexively re-run is disarmed exactly as
// thoroughly as one that was deleted. The cost arrives through habit, not code.
//
// Measured with `scripts/tsan-canary-rate.sh`, 5000 runs per arm, `-O0`, clang
// 20.1.2, on a 32-core Linux box:
//
//     shape                        unpinned          pinned to 2 CPUs
//     1 location x 1000 (the old)  11/5000  0.220%   35/5000  0.700%
//     1024 locations x 1            0/5000  0.000%    0/5000  0.000%
//     256 locations x 64 (this)     0/5000  0.000%    0/5000  0.000%
//
// Pinning makes the old shape worse, which is the wrong direction for CI: a
// two-core runner is the machine this has to be certain on.
//
// ## What the mechanism is NOT
//
// Stated because a wrong explanation here would send the next person somewhere
// expensive, and the obvious one is wrong. The first hypothesis was shadow-cell
// eviction -- two threads hammering one address churn the small fixed number of
// shadow cells per granule -- which predicts that MORE accesses on one address
// is worse. Measured over 1000 runs each, it is the opposite:
//
//     1 location x 1 increment       2.50% silent
//     1 location x 10                2.80% silent
//     1 location x 1000              0.20% silent
//     1 location x 100000            0.00% silent
//
// More work on one address is safer, not riskier, so eviction is refuted. And a
// single unsynchronised pair -- which by the standard is unambiguously a race,
// and which ThreadSanitizer decides on happens-before rather than on overlap --
// is the LEAST reliable shape of all, at 2.5%.
//
// No mechanism is claimed. What the numbers do show is which dimensions move it:
// the number of distinct LOCATIONS matters most (1024 locations touched once
// each is reliable where one location touched 1024 times is not), and repetition
// on a location helps too but needs orders of magnitude more of it. So this uses
// both, with margin in each, rather than the minimum of either. `Cells` and
// `Repeats` are the two knobs, and the rate script is how a change to them is
// judged -- a green run is not evidence, a rate over a few hundred runs is.
//
// ## Why the race is written the way it is
//
// Two threads writing the same non-atomic `int`s with no synchronisation between
// them. The latch is what makes the accesses genuinely concurrent rather than
// merely unordered: without it the first thread routinely finishes before the
// second starts.
//
// Do not "fix" this file. Do not add a mutex, an atomic, or a `volatile`. If a
// static analyser flags it, that analyser is working; exclude the file rather
// than repairing it.

#include <cstdio>
#include <latch>
#include <thread>

namespace
{

/// How many distinct locations the two threads race on. Deliberately non-atomic
/// and deliberately global, so no optimiser can privatise them into registers
/// and dissolve the race.
constexpr int Cells = 256;

/// How many times each thread sweeps the whole array.
constexpr int Repeats = 64;

int shared[Cells] {};

} // namespace

int main()
{
    // Both threads wait here, so the unsynchronised accesses genuinely overlap
    // rather than merely being unordered.
    std::latch start { 2 };

    auto const race = [&start] {
        start.arrive_and_wait();
        for (auto r = 0; r < Repeats; ++r)
            for (auto i = 0; i < Cells; ++i)
                shared[i] = shared[i] + 1;
    };

    std::thread a { race };
    std::thread b { race };
    a.join();
    b.join();

    // Summed and printed so a human reading a failed gate can see the program ran
    // at all, and so the values are used -- an unread global is a global an
    // optimiser may decide nobody needs. A total below the no-race figure is
    // increments actually lost, which is the race having happened; it is
    // evidence for the reader, never a verdict.
    auto total = 0;
    for (auto i = 0; i < Cells; ++i)
        total += shared[i];
    std::printf("tsan-canary: %d increments observed (expected %d if no race)\n",
                total,
                2 * Cells * Repeats);

    // Exit 0 ALWAYS. Whether this program failed is ThreadSanitizer's verdict to
    // deliver, through its own exit code, not this program's opinion of its own
    // arithmetic: a full count happens routinely even when the race is real, so
    // deciding it here would be its own false success. Uninstrumented, this
    // returns 0 and the gate refuses the run.
    return 0;
}
