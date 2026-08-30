// SPDX-License-Identifier: Apache-2.0
//
// A deliberate out-of-range `std::vector` subscript, whose ONLY job is to be
// caught by MSVC's debug runtime.
//
// ## Why this file contains a bug on purpose
//
// The value of a Windows Debug leg is not that it compiles -- it is that
// `_ITERATOR_DEBUG_LEVEL=2` traps invalidated iterators, out-of-range indexing
// and mismatched container iterators at the point they happen. A leg that builds
// and runs the suite while that runtime is silently absent buys nothing and
// reports green, which is the failure class this repository keeps a list about:
// the job ran, the artefact was fine, and the thing somebody was told is covered
// was not.
//
// `_ITERATOR_DEBUG_LEVEL` is not something the build states; it follows from
// `_DEBUG`, which follows from the runtime library flavour, which follows from
// `CMAKE_BUILD_TYPE` and `CMAKE_MSVC_RUNTIME_LIBRARY`. Any of those moving takes
// the checks away without touching a line of this project's code and without a
// single warning. So the checks are asserted at run time, by a program that must
// die, rather than assumed from the preset's name.
//
// This is `src/tests/TsanCanary.cpp`'s shape, deliberately: one idiom for "prove
// the tool would have gone red" rather than two.
//
// ## Why this is not a Catch2 case
//
// Because `catch_discover_tests` registers every case it finds, and a case that
// must abort would make `ctest` red by construction -- the guard working would be
// indistinguishable from the suite failing. CTest can express the inversion
// (`WILL_FAIL`), but the verdict still has to be read by something that can say
// WHY a green result is meaningless, which a boolean cannot. So this is a bare
// executable and `scripts/iterator-debug-gate.ps1` decides what its exit status
// means.
//
// Do not "fix" this file. Do not add a bounds check, an `.at()`, or a guard. If a
// static analyser flags it, that analyser is working; exclude the file rather
// than repairing it.

#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(_MSC_VER)
    #include <crtdbg.h>
#endif

int main()
{
#if defined(_MSC_VER)
    // Route the diagnostic to stderr and take the process down without a dialog.
    //
    // This is the load-bearing half of the file on CI and not defensive padding: a
    // debug assertion in a Windows GUI session pops a modal "Abort/Retry/Ignore"
    // box, and on a runner that means the job hangs until its timeout rather than
    // failing in a second. A canary that wedges CI is worse than no canary.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif

    // Sized from a runtime value and read past the end. Not a constant expression
    // and not a literal index: MSVC folds the obvious spelling away at compile
    // time, and a canary the optimiser deletes reports exactly what a missing
    // debug runtime reports.
    std::vector<int> values(4, 7);
    auto const index = static_cast<std::size_t>(std::atoi("9"));

    // With `_ITERATOR_DEBUG_LEVEL=2` this is `_STL_VERIFY`, which reports and
    // aborts. Without it, this is undefined behaviour that in practice reads
    // adjacent memory and keeps going -- which is the whole point: the two
    // outcomes are what tell the levels apart.
    auto const observed = values[index];

    std::printf("iterator-debug-canary: read %d past the end of a %zu-element vector "
                "and SURVIVED -- the debug runtime did not trap it\n",
                observed, values.size());

    // Exit 0 on survival, so the gate's verdict is unambiguous: this program
    // returning normally IS the failure being reported.
    return 0;
}
