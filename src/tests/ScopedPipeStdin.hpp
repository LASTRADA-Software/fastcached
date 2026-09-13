// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace FastCache::Testing
{

/// Replace standard input with the read end of a pipe for the duration of a case.
///
/// A pipe is never a terminal on any platform, so with it in place "is there a terminal" has one
/// answer whatever the runner connected stdin to. Asserting on the runner's own stdin would pass or
/// fail by whether ctest was started from a terminal.
///
/// Shared rather than copied, for `.agent/rules/testing.md`'s reason about test helpers: two copies
/// of this lived in the platform and CLI suites, and a fix reaching one of them reaches neither.
///
/// Windows swaps the process's standard handle (`SetStdHandle`), which is what
/// `StandardStreamsAreInteractive` reads there; POSIX `dup2`s descriptor 0.
struct ScopedPipeStdin
{
#if defined(_WIN32)
    ScopedPipeStdin():
        saved { ::GetStdHandle(STD_INPUT_HANDLE) },
        installed { ::CreatePipe(&readEnd, &writeEnd, nullptr, 0) != 0 && ::SetStdHandle(STD_INPUT_HANDLE, readEnd) != 0 }
    {
    }

    ~ScopedPipeStdin()
    {
        ::SetStdHandle(STD_INPUT_HANDLE, saved);
        if (readEnd != nullptr)
            ::CloseHandle(readEnd);
        if (writeEnd != nullptr)
            ::CloseHandle(writeEnd);
    }
#else
    ScopedPipeStdin():
        saved { ::dup(STDIN_FILENO) },
        installed { saved >= 0 && ::pipe(ends.data()) == 0 && ::dup2(ends[0], STDIN_FILENO) >= 0 }
    {
    }

    ~ScopedPipeStdin()
    {
        if (saved >= 0)
        {
            ::dup2(saved, STDIN_FILENO);
            ::close(saved);
        }
        for (auto const end: ends)
            if (end >= 0)
                ::close(end);
    }
#endif

    ScopedPipeStdin(ScopedPipeStdin const&) = delete;
    ScopedPipeStdin& operator=(ScopedPipeStdin const&) = delete;
    ScopedPipeStdin(ScopedPipeStdin&&) = delete;
    ScopedPipeStdin& operator=(ScopedPipeStdin&&) = delete;

#if defined(_WIN32)
    HANDLE saved { nullptr };
    HANDLE readEnd { nullptr };
    HANDLE writeEnd { nullptr };
#else
    int saved { -1 };
    std::array<int, 2> ends { -1, -1 };
#endif

    /// Whether the pipe really replaced stdin. Asserted by every case, or a failed setup would
    /// leave the runner's own stdin in place and the answer would depend on how ctest started.
    ///
    /// Declared LAST: its initializer uses every member above, which are initialized in order.
    bool installed { false };
};

} // namespace FastCache::Testing
