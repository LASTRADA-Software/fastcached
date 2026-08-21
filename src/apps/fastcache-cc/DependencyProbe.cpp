// SPDX-License-Identifier: Apache-2.0
#include "DependencyProbe.hpp"
#include "DirectManifest.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

namespace
{
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
        std::string out { path };
        std::ranges::replace(out, '\\', '/');
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

        // Normalized BEFORE anything is decided, exactly as BuildManifest does and
        // for the reason NormalizePath states: a driver echoes a path as resolved,
        // so `.`/`..` segments and mixed separators arrive verbatim. Skipping this
        // makes `<BUILDTREE>/../inc/a.hpp` and `<SRCROOT>/inc/a.hpp` two key
        // entries for one header — and, across two machines whose generators spell
        // an include directory differently, two keys for identical content.
        //
        // std::filesystem normalizes to the HOST's preferred separator, but every
        // test below is the LAYOUT's, so the layout's own convention is put back
        // first: on a Windows host a POSIX path returns backslash-separated, and
        // `IsAbsoluteForLayout` — which for a POSIX layout asks only about a
        // leading `/` — would then read an absolute toolchain path as relative and
        // hash it. That is the host coupling PathCanon.hpp forbids in as many
        // words ("Derived from the LAYOUT, never from the host").
        auto path = NormalizePath(raw);
        if (!PathCanon::IsWindowsLayout(layout))
            std::ranges::replace(path, '\\', '/');

        // Relative before absolute: a relative path lies under no root either, and
        // asking the absolute test second is what keeps it from being dropped with
        // the toolchain headers.
        if (!PathCanon::IsAbsoluteForLayout(path, layout))
            return WithPosixSeparators(path);

        // The classifier the manifest and the replay guard already use, so all
        // three agree on what "toolchain" means — including a vcpkg tree nested
        // under the build tree, which canonicalizes but is still the producing
        // machine's spelling of content the compiler identity already covers.
        if (IsToolchainHeader(path, layout))
            return {};

        // Canonicalize returns its input verbatim for a path it did not rewrite,
        // so inequality — not the spelling of a sentinel PathCanon keeps private —
        // is what says a token was produced.
        if (auto canon = PathCanon::Canonicalize(path, layout); canon.has_value() && *canon != path)
            return *std::move(canon);
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
        auto const line = text.substr(offset, lineEnd - offset);
        offset = terminated ? lineEnd + 1 : text.size();

        // Recognition is IncludeNotePath's, shared with ParseIncludePaths, and it
        // is anchored: this stream also carries preprocessed SOURCE, so a rule
        // that matched the marker mid-line would delete an ordinary line holding
        // that text from the bytes the key is computed over.
        auto const path = IncludeNotePath(line);
        if (!path.has_value())
        {
            out.preprocessed += whole;
            continue;
        }

        if (!path->empty())
            out.notePaths.emplace_back(*path);
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
