// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Terminal.hpp>

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
#if defined(_WIN32)

    /// @return true when `NO_COLOR` is present and non-empty. Uses the secure
    ///         CRT `getenv_s` so the build stays warning-clean under /WX.
    [[nodiscard]] bool NoColorRequested() noexcept
    {
        std::size_t size = 0;
        // getenv_s writes the value's length (including the NUL) into `size`:
        // 0 == not present, 1 == present but empty, >1 == present and non-empty.
        if (::getenv_s(&size, nullptr, 0, "NO_COLOR") != 0)
            return false;
        return size > 1;
    }

#else

    /// @return true when `NO_COLOR` is present and non-empty.
    [[nodiscard]] bool NoColorRequested() noexcept
    {
        char const* const value = std::getenv("NO_COLOR");
        return value != nullptr && value[0] != '\0';
    }

#endif
} // namespace

#if defined(_WIN32)

bool StdoutSupportsColor() noexcept
{
    if (NoColorRequested())
        return false;

    HANDLE const handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;

    // Only true consoles get color; a redirected pipe or file does not.
    if (::GetFileType(handle) != FILE_TYPE_CHAR)
        return false;

    DWORD mode = 0;
    if (!::GetConsoleMode(handle, &mode))
        return false;

    // Turn on ANSI escape interpretation. Harmless if it is already set; if
    // the call fails (legacy console) we fall back to no color.
    return ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

#else

bool StdoutSupportsColor() noexcept
{
    if (NoColorRequested())
        return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

#endif

} // namespace FastCache
