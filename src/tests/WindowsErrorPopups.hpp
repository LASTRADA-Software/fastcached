// SPDX-License-Identifier: Apache-2.0
#pragma once

#if defined(_MSC_VER)
    #include <cstdlib>
    #include <initializer_list>

    #include <windows.h>

    #include <crtdbg.h>
#endif

namespace FastCache::Testing
{

/// Route every Windows CRT diagnostic to stderr instead of a modal dialog.
///
/// **Load-bearing rather than defensive padding, and for two audiences.** A failed
/// `assert()`, a `_CrtDbgReport` or a GPF in a Windows *interactive* session pops an
/// "Abort/Retry/Ignore" box: a developer running `ctest` gets a wall of dialogs to
/// click through, and a headless runner gets a job that hangs until its timeout
/// rather than failing in a second. **A canary that wedges CI is worse than no
/// canary** — it converts "this guard fired, as designed" into "the build never
/// finished", and those are read completely differently.
///
/// It matters most in exactly the programs that are *supposed* to die. Several
/// canaries in this directory exist to be seen aborting (`read-slot-guard-canary`,
/// `empty-read-buffer-canary`, `reactor-teardown-canary`), so for them the dialog is
/// not an edge case — it is what happens on every successful run.
///
/// ## Why this is a shared header and not a copied block
///
/// It was copied, four times, and **it had already drifted**: `src/tests/test_main.cpp`
/// covered `_CRT_WARN` and called `SetErrorMode`, while `IteratorDebugCanary.cpp`
/// covered neither and guarded on `_MSC_VER` where the others guarded on `_WIN32`. That
/// is the shape `.agent/rules/testing.md` already records for shared test helpers —
/// three private copies of one scripted `ISocket` carried the same defect in two of
/// them. A suppression that is *almost* everywhere is indistinguishable from one that
/// is everywhere, right up to the run that hangs.
///
/// Guarded on `_MSC_VER` rather than `_WIN32`, because it is the MSVC CRT that is being
/// silenced: that predicate is true for `cl` and for `clang-cl`, which shares the CRT,
/// and false for a MinGW build that has no `<crtdbg.h>` to include. Elsewhere this is an
/// empty function rather than an `#if` at each call site, so a caller states the
/// intention unconditionally and no site can forget the guard.
inline void SuppressWindowsErrorPopups() noexcept
{
#if defined(_MSC_VER)
    // CRT debug reports: assert(), _CrtDbgReport, and the _STL_VERIFY checks that
    // `_ITERATOR_DEBUG_LEVEL=2` compiles in.
    for (int const report: { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN })
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }

    // abort(): write the message, do not raise the "send to Microsoft" dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // The OS half, which the CRT settings above do not cover: a general protection
    // fault or a missing-device error otherwise gets its own dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

} // namespace FastCache::Testing
