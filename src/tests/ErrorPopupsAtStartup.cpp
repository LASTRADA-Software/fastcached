// SPDX-License-Identifier: Apache-2.0
//
// Installs `SuppressWindowsErrorPopups()` in every executable this build defines, before
// anything in the process can assert (#1389).
//
// ## Why a translation unit the build attaches, and not a call in each `main`
//
// It was a call in each `main`, and four test executables had none: every target that
// links `Catch2::Catch2WithMain` runs Catch2's own `main`, which knows nothing of this. An
// `endo` test hung for 58 minutes on the "Debug Assertion Failed" box that left open --
// not a failure, a job that never finished. A suppression that depends on each `main`
// remembering is the copied block `WindowsErrorPopups.hpp` already records drifting, one
// level up. So `cmake/ErrorPopups.cmake` attaches this file to every executable by walking
// the build system, the way `cmake/Utf8CodePage.cmake` attaches the UTF-8 manifest, and
// `ctest -R error-popup-coverage` reads the generated link lines to prove it arrived.
//
// ## Why `init_seg(lib)`, and not a first statement in `main`
//
// A constructor in the `lib` segment runs after the CRT's own initialisation and before
// every USER static initialiser -- so a namespace-scope object in any other translation
// unit that asserts while it is being constructed is covered too, which no statement in
// `main` can promise. C4073 is the compiler saying exactly that, which is the intent here.
//
// ## Two policies
//
// A test executable suppresses always. A PRODUCT binary is compiled with
// `FASTCACHED_ERROR_POPUPS_ON_REQUEST` naming an environment variable, and suppresses only
// when that variable is present: a developer running a Debug `fastcached` by hand keeps
// the dialog and the chance to attach a debugger at the assert, while one a test fixture
// spawns inherits the variable every registered test carries.
//
// The variable is read with `GetEnvironmentVariableA` rather than through
// `Platform/Environment`, which is where this tree reads the environment, for two reasons
// that are both about WHEN and WHERE this runs: during static initialisation, ahead of
// every object that seam could be constructed in, and in binaries -- `fastcache-cc` -- that
// do not link the library it lives in. It decides one process-wide setting, once, and
// reaches nothing else.

#include "WindowsErrorPopups.hpp"

#if defined(_MSC_VER)
    #pragma warning(disable : 4073)
    #pragma init_seg(lib)

namespace
{

/// Whether this process is asked to suppress: always for a test executable, and for a
/// product binary only when the environment carries the variable it was compiled with.
/// @return True when the suppression should be installed.
[[nodiscard]] bool SuppressionRequested() noexcept
{
    #if defined(FASTCACHED_ERROR_POPUPS_ON_REQUEST)
    // A size query: a present variable answers its length plus the terminator, even when
    // empty, and an absent one answers 0.
    return ::GetEnvironmentVariableA(FASTCACHED_ERROR_POPUPS_ON_REQUEST, nullptr, 0) != 0;
    #else
    return true;
    #endif
}

/// Runs the suppression during static initialisation, ahead of user initialisers.
struct SuppressBeforeUserInitializers
{
    SuppressBeforeUserInitializers() noexcept
    {
        if (SuppressionRequested())
            FastCache::Testing::SuppressWindowsErrorPopups();
    }
};

SuppressBeforeUserInitializers const Suppressed;

} // namespace
#endif
