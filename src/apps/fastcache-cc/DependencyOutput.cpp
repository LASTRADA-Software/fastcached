// SPDX-License-Identifier: Apache-2.0
#include "DependencyOutput.hpp"
#include "DirectManifest.hpp"

#include <algorithm>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// Escape a path for a make rule: a space becomes `\ `.
    ///
    /// Unescaped, a path containing a space is read as TWO dependencies, and the
    /// second names a file that does not exist -- which make and Ninja answer by
    /// rebuilding this translation unit on every build, forever, with a zero exit
    /// code. The same non-convergence a stale depfile causes, reached by a
    /// different route.
    [[nodiscard]] std::string EscapeForMake(std::string_view path)
    {
        std::string out;
        out.reserve(path.size());
        for (auto const ch: path)
        {
            if (ch == ' ')
                out.push_back('\\');
            out.push_back(ch);
        }
        return out;
    }

    /// Sorted, de-duplicated copy.
    ///
    /// `/showIncludes` names a header once per inclusion SITE and a depfile can
    /// repeat one too, so the raw probe output is not a set. Emitting the repeats
    /// is harmless to make but makes the record needlessly large, and the ordering
    /// is a property of the driver rather than of the translation unit -- the same
    /// reasoning `KeyDependencySet` applies one level up.
    [[nodiscard]] std::vector<std::string> Unique(std::span<std::string const> paths)
    {
        std::vector<std::string> out { paths.begin(), paths.end() };
        std::ranges::sort(out);
        auto const duplicates = std::ranges::unique(out);
        out.erase(duplicates.begin(), duplicates.end());
        return out;
    }
} // namespace

std::string RenderDepFile(std::string_view target, std::span<std::string const> dependencyPaths)
{
    auto const deps = Unique(dependencyPaths);

    std::string out { target };
    out += ':';
    for (auto const& dep: deps)
    {
        // One per continued line, as -MD writes it. A single very long line is
        // legal but some make implementations cap line length, and the continued
        // form is what every real depfile looks like.
        out += " \\\n  ";
        out += EscapeForMake(dep);
    }
    out += '\n';
    return out;
}

std::string RenderShowIncludes(std::span<std::string const> dependencyPaths)
{
    std::string out;
    for (auto const& dep: Unique(dependencyPaths))
    {
        // The marker comes from DirectManifest, which is where the READING side
        // gets it. A second spelling here is how a producer and its parser drift --
        // and this pair has to agree byte-for-byte, since Ninja matches the prefix.
        out += IncludeNoteMarker;
        out += ' ';
        out += dep;
        out += "\r\n";
    }
    return out;
}

} // namespace FastCache::Cc
