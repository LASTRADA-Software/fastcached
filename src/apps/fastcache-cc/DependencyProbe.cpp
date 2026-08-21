// SPDX-License-Identifier: Apache-2.0
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"

#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// Strip a trailing `\r` so a note is recognised on either line ending.
    /// @param line One line, without its `\n`.
    /// @return The line without a CR terminator.
    [[nodiscard]] std::string_view WithoutCarriageReturn(std::string_view line) noexcept
    {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return line;
    }

    /// Trim ASCII blanks from both ends of a note's path span.
    /// @param path The text following the note marker.
    /// @return The path proper.
    [[nodiscard]] std::string_view TrimBlanks(std::string_view path) noexcept
    {
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
            path.remove_prefix(1);
        while (!path.empty() && (path.back() == ' ' || path.back() == '\t'))
            path.remove_suffix(1);
        return path;
    }

    /// Rewrite backslash separators to forward slashes.
    ///
    /// Only ever applied to a relative path being folded into the key. Canonical
    /// tokens already arrive in this form (PathCanon normalizes the tail), so
    /// without it a `inc\a.hpp` and an `inc/a.hpp` naming the same file would key
    /// as two different dependencies.
    /// @param path A relative dependency path.
    /// @return The path with `/` separators.
    [[nodiscard]] std::string WithPosixSeparators(std::string_view path)
    {
        std::string out;
        out.reserve(path.size());
        for (char const c: path)
            out.push_back(c == '\\' ? '/' : c);
        return out;
    }

    /// The portable form of one raw dependency path, or nothing when this machine
    /// must not hash it. See the header for why each branch is load-bearing.
    /// @param raw    A dependency path as the compiler spelled it.
    /// @param layout This machine's roots.
    /// @return The form to hash, or an empty string to drop the path.
    [[nodiscard]] std::string PortableForm(std::string_view raw, PathCanon::Layout const& layout)
    {
        if (raw.empty())
            return {};

        auto canon = PathCanon::Canonicalize(raw, layout);
        // Canonicalize returns its input verbatim for a path under neither root,
        // so a sentinel is what says the path was actually rewritten.
        if (canon.has_value() && canon->starts_with('<'))
            return *std::move(canon);

        // Relative before absolute: a relative path lies under no root either, and
        // asking the absolute test second is what keeps it from being dropped with
        // the toolchain headers.
        if (!PathCanon::IsAbsoluteForLayout(raw, layout))
            return WithPosixSeparators(raw);
        return {};
    }
} // namespace

ProbeText SplitIncludeNotes(std::string_view text)
{
    ProbeText out;
    out.preprocessed.reserve(text.size());

    std::size_t offset = 0;
    while (offset < text.size())
    {
        auto lineEnd = text.find('\n', offset);
        auto const terminated = lineEnd != std::string_view::npos;
        if (!terminated)
            lineEnd = text.size();

        // The line WITH its terminator, so a non-note is reproduced byte-for-byte.
        auto const whole = text.substr(offset, (terminated ? lineEnd + 1 : lineEnd) - offset);
        auto const line = WithoutCarriageReturn(text.substr(offset, lineEnd - offset));
        offset = terminated ? lineEnd + 1 : text.size();

        auto const marker = line.find(IncludeNoteMarker);
        if (marker == std::string_view::npos)
        {
            out.preprocessed += whole;
            continue;
        }

        if (auto const path = TrimBlanks(line.substr(marker + IncludeNoteMarker.size())); !path.empty())
            out.notePaths.emplace_back(path);
    }
    return out;
}

std::vector<std::string> KeyDependencySet(std::span<std::string const> rawPaths, PathCanon::Layout const& layout)
{
    std::vector<std::string> out;
    out.reserve(rawPaths.size());
    for (auto const& raw: rawPaths)
        if (auto portable = PortableForm(raw, layout); !portable.empty())
            out.push_back(std::move(portable));

    // Byte-wise, so the order is a property of the data rather than of the
    // machine's locale — two machines must produce the same key from the same set.
    std::ranges::sort(out);
    auto const duplicates = std::ranges::unique(out);
    out.erase(duplicates.begin(), duplicates.end());
    return out;
}

} // namespace FastCache::Cc
