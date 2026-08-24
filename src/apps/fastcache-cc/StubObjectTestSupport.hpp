// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace FastCache::Cc::Test
{

/// Write a stub object wherever an argv asks for one, for a fake compiler.
///
/// Honours BOTH families' output spellings, fused: `-o<path>` and `/Fo<path>`.
///
/// Shared rather than written per fake, because three of them modelled only the
/// GNU convention and that is precisely what hid a real bug: the compile worker
/// hard-coded `-o`, which `cl` does not accept, so MSVC wrote its object
/// elsewhere and the worker refused the job -- and every fake passed happily,
/// because each one only knew the flag the worker was already emitting. A fake
/// that models one family cannot fail for the other.
///
/// This header used to carry `UniqueScratchPath` as well, and was named for it.
/// That function is `tests/ScratchPath.hpp` now: living here put it out of reach
/// of the library test binary, which then re-derived it -- see that header. What
/// is left is genuinely launcher-specific, because it models a compiler's argv.
///
/// @param argv The command line the worker built.
/// @param contents Bytes to write, so a case can tell one object from another.
inline void WriteStubObject(std::span<std::string const> argv, std::string_view contents = "OBJECT")
{
    for (auto const& arg: argv)
    {
        for (auto const prefix: { std::string_view { "-o" }, std::string_view { "/Fo" } })
        {
            if (!arg.starts_with(prefix) || arg.size() == prefix.size())
                continue;
            std::ofstream { arg.substr(prefix.size()), std::ios::binary } << contents;
            break;
        }
    }
}

} // namespace FastCache::Cc::Test
