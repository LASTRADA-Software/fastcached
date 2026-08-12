// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/UsageDoc.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

/// Helpers shared by the tests that assert properties of rendered usage text.
///
/// These read the *output* rather than the document, because that is what the
/// properties are about: that color never disturbs alignment, and that one
/// section shares one column. Header-only, so the launcher's test binary — which
/// links no FastCache library — can use them too.
namespace FastCache::Testing
{

/// Remove every ANSI SGR escape, so colored output can be compared against plain
/// output character for character.
/// @param text Possibly-colored text.
/// @return `text` with all escape sequences removed.
[[nodiscard]] inline std::string StripAnsi(std::string_view text)
{
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '\x1b')
        {
            out += text[i];
            continue;
        }
        while (i < text.size() && text[i] != 'm')
            ++i;
    }
    return out;
}

/// Split rendered output into its visual lines.
///
/// Owning strings, not views: the natural call is `UsageLines(RenderUsage(...))`,
/// and views into that temporary would dangle at the end of the expression.
/// The empty segment the final newline produces is dropped, so the result is
/// one entry per printed line.
/// @param text The rendered text.
/// @return One entry per line.
[[nodiscard]] inline std::vector<std::string> UsageLines(std::string_view text)
{
    std::vector<std::string> lines;
    ForEachLine(text, [&](std::string_view line) { lines.emplace_back(line); });
    if (!lines.empty() && lines.back().empty())
        lines.pop_back();
    return lines;
}

/// Column (0-based) at which the description begins on an aligned line: the
/// first non-space character after the run of two or more spaces following the
/// term. A single space *inside* a term ("--help, -h") is not the gap.
/// @param line One rendered line, with escapes already stripped.
/// @return The description column, or `line.size()` when there is no gap.
[[nodiscard]] inline std::size_t DescriptionColumn(std::string_view line)
{
    auto i = std::size_t { 2 }; // past the leading indent
    while (i + 1 < line.size() && !(line[i] == ' ' && line[i + 1] == ' '))
        ++i;
    while (i < line.size() && line[i] == ' ')
        ++i;
    return i;
}

} // namespace FastCache::Testing
