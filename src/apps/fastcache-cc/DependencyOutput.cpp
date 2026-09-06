// SPDX-License-Identifier: Apache-2.0
#include "DependencyOutput.hpp"

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

std::string RenderShowIncludes(std::span<std::string const> dependencyPaths, std::string_view marker)
{
    std::string out;
    for (auto const& dep: Unique(dependencyPaths))
    {
        // The marker is the CALLER's, and this file deliberately spells none of its
        // own. It used to write `IncludeNoteMarker` -- the reading side's constant --
        // under a comment saying a second spelling here is how a producer and its
        // parser drift. That was right about the two parties it could see and blind
        // to the third: Ninja does not match `IncludeNoteMarker`, it matches
        // `msvc_deps_prefix`, which CMake took from the actual compiler and which is
        // localized on a Visual Studio carrying a language pack (#700).
        //
        // So the invariant survives in a stronger form. There is no literal here to
        // drift with, the one definition is `IncludeNoteMarker`, and the only way
        // these lines carry anything else is an operator naming it. See
        // DependencyOutput.hpp for the ninja measurement behind that.
        out += marker;
        out += ' ';
        out += dep;
        out += "\r\n";
    }
    return out;
}

} // namespace FastCache::Cc
