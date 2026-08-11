// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Terminal.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace FastCache
{

namespace
{
    /// @return true when `NO_COLOR` is present and non-empty. Per the NO_COLOR
    ///         convention, a variable that is set but *empty* does not disable
    ///         color, which is why this tests the value rather than presence.
    [[nodiscard]] bool NoColorRequested()
    {
        auto const value = ReadEnvironmentVariable("NO_COLOR");
        return value.has_value() && !value->empty();
    }
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
