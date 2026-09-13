// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/Terminal.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <new>
#include <string>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <termios.h>
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

    /// The code page Windows numbers UTF-8 with.
    constexpr auto Utf8CodePage = std::uint32_t { 65001 };

    /// The codeset spellings that name UTF-8, once case and hyphens are dropped.
    constexpr auto Utf8Codesets = std::to_array<std::string_view>({ "utf8" });

    /// @return @p locale's codeset, lowercased and without hyphens; empty when it names none.
    [[nodiscard]] std::string NormalisedCodeset(std::string_view locale)
    {
        auto const dot = locale.find('.');
        if (dot == std::string_view::npos)
            return {};
        auto codeset = locale.substr(dot + 1);
        codeset = codeset.substr(0, codeset.find('@'));
        auto normalised = std::string {};
        for (auto const character: codeset)
        {
            if (character == '-')
                continue;
            normalised.push_back((character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a')
                                                                        : character);
        }
        return normalised;
    }
} // namespace

TerminalTextEncoding EncodingFromLocaleVariables(std::optional<std::string_view> lcAll,
                                                 std::optional<std::string_view> lcCtype,
                                                 std::optional<std::string_view> lang) noexcept
{
    for (auto const& variable: { lcAll, lcCtype, lang })
    {
        if (!variable.has_value() || variable->empty())
            continue;
        auto const codeset = NormalisedCodeset(*variable);
        return std::ranges::find(Utf8Codesets, std::string_view { codeset }) != Utf8Codesets.end()
                   ? TerminalTextEncoding::Utf8
                   : TerminalTextEncoding::Other;
    }
    return TerminalTextEncoding::Unknown;
}

TerminalTextEncoding EncodingFromConsoleOutputCodePage(std::optional<std::uint32_t> codePage) noexcept
{
    if (!codePage.has_value())
        return TerminalTextEncoding::Unknown;
    return *codePage == Utf8CodePage ? TerminalTextEncoding::Utf8 : TerminalTextEncoding::Other;
}

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

TerminalTextEncoding DetectTerminalTextEncoding()
{
    // `GetConsoleOutputCP` answers 0 when this process has no console to ask.
    auto const codePage = ::GetConsoleOutputCP();
    return EncodingFromConsoleOutputCodePage(codePage == 0 ? std::nullopt : std::optional<std::uint32_t> { codePage });
}

bool StandardStreamsAreInteractive() noexcept
{
    auto const isConsole = [](DWORD which) {
        HANDLE const handle = ::GetStdHandle(which);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE || ::GetFileType(handle) != FILE_TYPE_CHAR)
            return false;
        DWORD mode = 0;
        return ::GetConsoleMode(handle, &mode) != 0;
    };
    return isConsole(STD_INPUT_HANDLE) && isConsole(STD_OUTPUT_HANDLE);
}

struct SavedTerminalModes::Modes
{
    HANDLE input { nullptr };
    HANDLE output { nullptr };
    DWORD inputMode { 0 };
    DWORD outputMode { 0 };
    UINT inputCodePage { 0 };
    UINT outputCodePage { 0 };
};

std::optional<SavedTerminalModes> SavedTerminalModes::Capture() noexcept
{
    if (!StandardStreamsAreInteractive())
        return std::nullopt;
    try
    {
        auto modes = std::make_shared<Modes>();
        modes->input = ::GetStdHandle(STD_INPUT_HANDLE);
        modes->output = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (::GetConsoleMode(modes->input, &modes->inputMode) == 0
            || ::GetConsoleMode(modes->output, &modes->outputMode) == 0)
            return std::nullopt;
        modes->inputCodePage = ::GetConsoleCP();
        modes->outputCodePage = ::GetConsoleOutputCP();
        return SavedTerminalModes { std::move(modes) };
    }
    catch (std::bad_alloc const&)
    {
        return std::nullopt;
    }
}

void SavedTerminalModes::Apply(std::string_view resets) const noexcept
{
    while (!resets.empty())
    {
        DWORD written = 0;
        if (::WriteFile(_modes->output, resets.data(), static_cast<DWORD>(resets.size()), &written, nullptr) == 0
            || written == 0)
            break;
        resets.remove_prefix(written);
    }
    ::SetConsoleMode(_modes->input, _modes->inputMode);
    ::SetConsoleMode(_modes->output, _modes->outputMode);
    // 0 is what both getters answer when there was no console page to read.
    if (_modes->outputCodePage != 0)
        ::SetConsoleOutputCP(_modes->outputCodePage);
    if (_modes->inputCodePage != 0)
        ::SetConsoleCP(_modes->inputCodePage);
}

#else

bool StdoutSupportsColor() noexcept
{
    if (NoColorRequested())
        return false;
    return ::isatty(STDOUT_FILENO) != 0;
}

bool StandardStreamsAreInteractive() noexcept
{
    return ::isatty(STDIN_FILENO) != 0 && ::isatty(STDOUT_FILENO) != 0;
}

struct SavedTerminalModes::Modes
{
    termios input {};
};

std::optional<SavedTerminalModes> SavedTerminalModes::Capture() noexcept
{
    if (!StandardStreamsAreInteractive())
        return std::nullopt;
    try
    {
        auto modes = std::make_shared<Modes>();
        if (::tcgetattr(STDIN_FILENO, &modes->input) != 0)
            return std::nullopt;
        return SavedTerminalModes { std::move(modes) };
    }
    catch (std::bad_alloc const&)
    {
        return std::nullopt;
    }
}

void SavedTerminalModes::Apply(std::string_view resets) const noexcept
{
    while (!resets.empty())
    {
        auto const written = ::write(STDOUT_FILENO, resets.data(), resets.size());
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            break;
        resets.remove_prefix(static_cast<std::size_t>(written));
    }
    // TCSANOW, not the TCSAFLUSH a UI's own teardown uses: FLUSH waits for pending output to drain,
    // and a process ending because something is stuck must not wait on a terminal that stopped
    // reading.
    ::tcsetattr(STDIN_FILENO, TCSANOW, &_modes->input);
}

TerminalTextEncoding DetectTerminalTextEncoding()
{
    auto const lcAll = ReadEnvironmentVariable("LC_ALL");
    auto const lcCtype = ReadEnvironmentVariable("LC_CTYPE");
    auto const lang = ReadEnvironmentVariable("LANG");
    auto const view = [](std::optional<std::string> const& value) {
        return value.has_value() ? std::optional<std::string_view> { *value } : std::nullopt;
    };
    return EncodingFromLocaleVariables(view(lcAll), view(lcCtype), view(lang));
}

#endif

SavedTerminalModes::SavedTerminalModes(std::shared_ptr<Modes const> modes) noexcept:
    _modes { std::move(modes) }
{
}

} // namespace FastCache
