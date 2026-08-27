// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Utf8.hpp>
#include <FastCache/Platform/NarrowText.hpp>

#include <limits>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

#if defined(_WIN32)
// The one place both spellings are visible, so this is where they are tied
// together. NarrowText.hpp states 65001 itself rather than including <windows.h>
// -- which is what keeps it usable on every platform -- and a header that repeats
// a number owes the reader a guarantee it is still the right one.
static_assert(Utf8CodePage == CP_UTF8);
#endif

namespace
{
    /// The code page a tool this process ran writes its narrow text in.
    ///
    /// Not exported: it is one half of `NarrowTextPolicy` and has no caller that
    /// wants it alone. See that struct for why the console output code page is the
    /// answer and the child's own active code page is not.
    ///
    /// Two answers rather than one, because a build does not always have a console.
    /// A `cl.exe` with one writes through `GetConsoleOutputCP()` -- measured -- and
    /// one without falls back to its own active code page, which for a binary
    /// carrying no `activeCodePage` manifest is the SYSTEM default. Answering
    /// nothing in that case would be worse than a guess and not better: it makes
    /// every non-ASCII path a tool reports unreadable, and the launcher then
    /// declines to cache a translation unit it cached yesterday, in silence, on an
    /// MSBuild or IDE build where the `chcp` recovery does not even exist.
    ///
    /// @return The code page a tool would have used on Windows; `std::nullopt`
    ///         elsewhere, where nothing is transcoded at all.
    [[nodiscard]] std::optional<std::uint32_t> ToolTextCodePage() noexcept
    {
#if defined(_WIN32)
        // 0 is what a process with no console answers. It is not a code page --
        // `MultiByteToWideChar` would read it as CP_ACP, which is THIS process's
        // page and has nothing to do with what the tool wrote -- so it falls through
        // rather than being passed on.
        if (auto const console = static_cast<std::uint32_t>(::GetConsoleOutputCP()); console != 0)
            return console;

        // The SYSTEM default, deliberately, and not `GetACP()`: this process
        // declares UTF-8, so its own answer is the one thing that cannot be what a
        // child without that declaration got.
        DWORD systemAnsi = 0;
        if (::GetLocaleInfoEx(LOCALE_NAME_SYSTEM_DEFAULT,
                              LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
                              reinterpret_cast<LPWSTR>(&systemAnsi),
                              static_cast<int>(sizeof systemAnsi / sizeof(wchar_t)))
            == 0)
            return std::nullopt;

        // 0 is CP_ACP again, which a locale with no ANSI page (UTF-16-only, such as
        // some Indic ones) legitimately reports. Nothing rather than this process's
        // own page, for the reason above.
        if (systemAnsi == 0)
            return std::nullopt;
        return static_cast<std::uint32_t>(systemAnsi);
#else
        return std::nullopt;
#endif
    }
} // namespace

std::optional<std::uint32_t> ActiveCodePage() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetACP());
#else
    return std::nullopt;
#endif
}

bool NarrowTextIsUtf8() noexcept
{
    auto const codePage = ActiveCodePage();
    return !codePage.has_value() || *codePage == Utf8CodePage;
}

NarrowTextPolicy HostNarrowTextPolicy() noexcept
{
    // `pathsAreUtf8` is deliberately NOT `NarrowTextIsUtf8()`: that answers true
    // where nothing is transcoded, which is exactly the host on which narrow bytes
    // need not be UTF-8 to name a file. Asking for the code page to BE UTF-8 is the
    // question a path conversion has.
    return NarrowTextPolicy { .pathsAreUtf8 = ActiveCodePage() == Utf8CodePage, .toolCodePage = ToolTextCodePage() };
}

std::optional<std::string> Utf8FromNarrowText(std::string_view text, std::optional<std::uint32_t> codePage)
{
    if (IsValidUtf8(text))
        return std::string { text };

    if (!codePage.has_value() || *codePage == Utf8CodePage)
        return std::nullopt;

#if defined(_WIN32)
    // The Win32 conversions are `int`-sized. A depfile past 2 GiB is not a thing
    // that happens, but a silent truncation to a negative length is a wrong answer
    // rather than a refused one, so it is refused.
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::nullopt;

    auto const narrowLength = static_cast<int>(text.size());

    // MB_ERR_INVALID_CHARS and WC_ERR_INVALID_CHARS on both legs, so what the named
    // page cannot express is a refusal rather than a U+FFFD quietly substituted
    // into a filename. It catches less than it sounds like -- a single-byte page
    // decodes nearly everything, and Windows even maps CP-1252's five unassigned
    // bytes to the matching C1 controls -- which is precisely why there is one
    // candidate page and never a ladder of them.
    auto const wideLength = ::MultiByteToWideChar(*codePage, MB_ERR_INVALID_CHARS, text.data(), narrowLength, nullptr, 0);
    if (wideLength <= 0)
        return std::nullopt;

    std::wstring wide;
    wide.resize(static_cast<std::size_t>(wideLength));
    if (::MultiByteToWideChar(*codePage, MB_ERR_INVALID_CHARS, text.data(), narrowLength, wide.data(), wideLength) <= 0)
        return std::nullopt;

    auto const utf8Length =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
    if (utf8Length <= 0)
        return std::nullopt;

    std::string utf8;
    utf8.resize(static_cast<std::size_t>(utf8Length));
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength, utf8.data(), utf8Length, nullptr, nullptr)
        <= 0)
        return std::nullopt;

    return utf8;
#else
    // Unreachable through `HostNarrowTextPolicy()`, which names no code page here,
    // and spelled out rather than left to fall off the end: there is no transcoder
    // on this platform, so a code page a caller names is a request this build
    // cannot honour, and saying so is not the same as saying the text is fine.
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> PathFromNarrowText(std::string_view text) noexcept
{
    // The only `catch` in this project's own code, and deliberately the only one:
    // the standard library states this failure by throwing and there is no
    // `error_code` overload of the constructor to ask instead, so the choice is one
    // guard here or a `try` at every call site that ever sees foreign bytes.
    try
    {
        return std::filesystem::path { text };
    }
    catch (std::exception const&)
    {
        return std::nullopt;
    }
}

} // namespace FastCache
