// SPDX-License-Identifier: Apache-2.0
//
// Catch2 entry point for the FastCacheTest binary. We provide a custom main
// (rather than linking Catch2WithMain) so future work can register CLI flags
// for connecting tests to non-default test seams (test-env paths, etc.).

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#if defined(_WIN32)
    #include <crtdbg.h>
    #include <stdlib.h>
    #include <windows.h>

namespace
{

/// Route every Windows / MSVC CRT diagnostic to stderr instead of a
/// modal dialog. Without this, a failed `assert()` or a CRT debug
/// check pops up a UI prompt and headless CI / `ctest` runs hang
/// forever waiting for a button click.
void SuppressWindowsErrorPopups() noexcept
{
    // CRT debug reports (assert, _CrtDbgReport, etc.).
    for (int const report: { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN })
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    // abort()-on-crash: do not pop "send to Microsoft" dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Windows error mode: suppress GPF + critical-error dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}

} // namespace
#endif

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    SuppressWindowsErrorPopups();
#endif
    Catch::Session session;
    auto const cliReturn = session.applyCommandLine(argc, argv);
    if (cliReturn != 0)
        return cliReturn;
    return session.run();
}
