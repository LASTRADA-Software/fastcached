// SPDX-License-Identifier: Apache-2.0
// Catch2 entry point for the fastcache-cc-tests binary.

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#if defined(_WIN32)
    #include <crtdbg.h>
    #include <stdlib.h>
    #include <windows.h>

namespace
{
/// Route CRT/Windows diagnostics to stderr so a failed assert does not pop a
/// modal dialog that hangs headless ctest runs.
void SuppressWindowsErrorPopups() noexcept
{
    for (int const report: { _CRT_ASSERT, _CRT_ERROR, _CRT_WARN })
    {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
}
} // namespace
#endif

int main(int argc, char* argv[])
{
#if defined(_WIN32)
    SuppressWindowsErrorPopups();
#endif
    return Catch::Session().run(argc, argv);
}
