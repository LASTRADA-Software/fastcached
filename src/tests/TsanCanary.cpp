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
// ## Why the race is written the way it is
//
// Two threads writing one non-atomic `int` with no synchronisation between them.
// The latch is what makes the report reliable rather than likely: without it the
// first thread routinely finishes before the second starts, and two accesses
// that never overlap in time still race by the standard but give ThreadSanitizer
// no interleaving to observe. A canary that reports only most of the time is a
// flaky build, which is worse than no canary at all -- it teaches people to
// re-run a red gate.
//
// Do not "fix" this file. Do not add a mutex, an atomic, or a `volatile`. If a
// static analyser flags it, that analyser is working; exclude the file rather
// than repairing it.

#include <cstdio>
#include <latch>
#include <thread>

namespace
{

/// The raced-upon location. Deliberately non-atomic and deliberately global, so
/// no optimiser can privatise it into a register and dissolve the race.
int shared = 0;

/// How many times each thread touches `shared`. Small: ThreadSanitizer decides
/// on happens-before rather than on repetition, so one overlapping pair is
/// enough and a long loop only slows the gate down.
constexpr int Iterations = 1000;

} // namespace

int main()
{
    // Both threads wait here, so the two unsynchronised accesses genuinely
    // overlap rather than merely being unordered.
    std::latch start { 2 };

    auto const race = [&start] {
        start.arrive_and_wait();
        for (auto i = 0; i < Iterations; ++i)
            shared = shared + 1;
    };

    std::thread a { race };
    std::thread b { race };
    a.join();
    b.join();

    // Printed so a human reading a failed gate can see the program ran at all,
    // and so the value is used -- an unread global is a global an optimiser may
    // decide nobody needs.
    std::printf("tsan-canary: %d increments observed (expected %d if no race)\n", shared, 2 * Iterations);

    // Exit 0 ALWAYS. Whether this program failed is ThreadSanitizer's verdict to
    // deliver, through its own exit code, not this program's opinion of its own
    // arithmetic: `shared == 2000` happens routinely even when the race is real,
    // so deciding it here would be its own false success. Uninstrumented, this
    // returns 0 and the gate refuses the run.
    return 0;
}
