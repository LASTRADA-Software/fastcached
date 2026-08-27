// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/NarrowText.hpp>

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

} // namespace FastCache
