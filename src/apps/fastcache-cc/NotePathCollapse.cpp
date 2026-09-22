// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "NotePathCollapse.hpp"

#include <FastCache/CompileCache/PathCanon.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace FastCache::Cc
{

namespace
{

    /// Either separator, because a compiler emits both and often in one path.
    [[nodiscard]] constexpr bool IsSeparator(char c) noexcept
    {
        return c == '/' || c == '\\';
    }

    /// True when `text` carries `..` as a whole path SEGMENT.
    ///
    /// The distinction is the point: `D:\src\a..b\x.hpp` contains `..` and has no `..` segment, so
    /// a `contains("..")` test would send it down the slow path and hand back a re-separated
    /// spelling for no reason. Both ends must land on a boundary.
    [[nodiscard]] bool HasDotDotSegment(std::string_view text)
    {
        auto at = text.find("..");
        while (at != std::string_view::npos)
        {
            auto const after = at + 2;
            bool const opensSegment = at == 0 || IsSeparator(text[at - 1]);
            bool const closesSegment = after == text.size() || IsSeparator(text[after]);
            if (opensSegment && closesSegment)
                return true;
            at = text.find("..", at + 1);
        }
        return false;
    }

    /// A copy with every `\` folded to `/`.
    [[nodiscard]] std::string Folded(std::string_view text)
    {
        std::string folded { text };
        std::ranges::replace(folded, '\\', '/');
        return folded;
    }

    /// A copy with every `/` written back as `\`.
    [[nodiscard]] std::string Unfolded(std::string_view text)
    {
        std::string native { text };
        std::ranges::replace(native, '/', '\\');
        return native;
    }

} // namespace

std::string CollapseRelativeSegments(std::string_view path)
{
    // The overwhelmingly common case, and the reason this is cheap enough to run over every note
    // of every compile: nothing to collapse means nothing to re-spell either.
    if (!HasDotDotSegment(path))
        return std::string { path };

    bool const wasNative = path.contains('\\');
    auto const folded = Folded(path);

    // The anchor is held aside so the lexical pass never sees it, which is what makes the result
    // the same on either host. `std::filesystem` reads a drive specifier as an ordinary filename
    // on POSIX, so `C:/../x.hpp` would normalize to a bare `x.hpp` there and to `C:/x.hpp` on
    // Windows -- and a drive root cannot be ascended past, which is a property of the path rather
    // than of the machine reading it. The UNC case is the same argument for a different prefix:
    // `lexically_normal` collapses a leading `//` on POSIX and keeps it on Windows.
    std::string_view tail { folded };
    std::string_view anchor;
    if (tail.starts_with("//") && !tail.starts_with("///"))
    {
        anchor = tail.substr(0, 1);
        tail.remove_prefix(1);
    }
    else if (tail.size() >= 2 && PathCanon::IsDriveLetter(tail[0]) && tail[1] == ':')
    {
        anchor = tail.substr(0, 2);
        tail.remove_prefix(2);
    }

    // `NormalizePath` is the one lexical pass in the launcher; it normalizes to the HOST's
    // separator, so fold its answer back. The tail is `/`-only by construction, which is what lets
    // that pass collapse a Windows path on a POSIX host at all.
    auto const collapsed = Folded(NormalizePath(tail));

    // A genuinely relative `../../x.hpp` has nothing lexical left to resolve. Returning the input
    // rather than the rewrite keeps the byte-exactness promise for a caller who gained nothing.
    if (HasDotDotSegment(collapsed))
        return std::string { path };

    auto const rejoined = std::string { anchor } + collapsed;
    return wasNative ? Unfolded(rejoined) : rejoined;
}

std::string CollapseNotePaths(std::string_view text, PathCanon::Grammar grammar, std::string_view marker)
{
    // Only Ninja's `/showIncludes` reader carries the length check; its depfile reader does not.
    // One gate here beats the same `if` at every call site.
    if (grammar != PathCanon::Grammar::ShowIncludes)
        return std::string { text };

    // `PathCanon`'s span finder matches the canonical marker and nothing else, so a localized
    // build's notes are invisible to it unless the marker is normalized in first. Both rewrites
    // short-circuit to a copy when the two spellings are equal, which is every English build.
    auto const canonical = PathCanon::NormalizeIncludeNoteMarker(text, marker);
    auto const collapsed =
        PathCanon::RewritePaths(canonical, grammar, [](std::string_view span) { return CollapseRelativeSegments(span); });
    return PathCanon::RestoreIncludeNoteMarker(collapsed, marker);
}

} // namespace FastCache::Cc
