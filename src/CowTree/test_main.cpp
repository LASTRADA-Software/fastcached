// SPDX-License-Identifier: Apache-2.0
//
// Catch2 entry point for the CowTreeTests binary.

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>

#include <tests/WindowsErrorPopups.hpp>

int main(int argc, char* argv[])
{
    FastCache::Testing::SuppressWindowsErrorPopups();
    Catch::Session session;
    auto const cliReturn = session.applyCommandLine(argc, argv);
    if (cliReturn != 0)
        return cliReturn;
    return session.run();
}
