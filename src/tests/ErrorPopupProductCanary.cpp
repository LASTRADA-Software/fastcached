// SPDX-License-Identifier: Apache-2.0
//
// Reports where this process's CRT would send a failed assert, and exits (#1389).
//
// Built with the PRODUCT policy (`FASTCACHED_ERROR_POPUPS_POLICY on-request`), so the
// startup object installs the suppression only when the environment carries the variable.
// `scripts/error-popup-product-gate.cmake` runs it twice -- with the variable and without --
// and requires the two answers to differ in the right direction: routed to a file when
// asked, left on the dialog when not, which is what a developer running a Debug product
// binary by hand keeps.
//
// It REPORTS the mode rather than asserting. The other canary proves an assert ends the
// process; this one has to show the NOT-suppressed direction too, and an assert there would
// open the dialog it is describing -- on a desktop that waits for a click, and on a runner
// with no desktop it proves nothing either way. `_CRTDBG_REPORT_MODE` asks the question
// without firing anything.

#include <cstdio>

#include <crtdbg.h>

int main()
{
    int const mode = _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_REPORT_MODE);
    // Whole lines through `puts` rather than a `printf` format: the analyser asks for
    // `std::print` over `printf`, and this answer needs no formatting at all.
    if ((mode & _CRTDBG_MODE_WNDW) != 0)
        std::puts("assert-report-mode=window");
    else if ((mode & _CRTDBG_MODE_FILE) != 0)
        std::puts("assert-report-mode=file");
    else
        std::puts("assert-report-mode=other");
    return 0;
}
