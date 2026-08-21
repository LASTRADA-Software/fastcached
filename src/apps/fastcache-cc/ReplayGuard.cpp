// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "ReplayGuard.hpp"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// A region grammar paired with the parser that names the dependency paths
    /// inside it.
    struct DependencyGrammar
    {
        PathCanon::Grammar grammar;                            ///< The grammar this row describes.
        std::vector<std::string> (*extract)(std::string_view); ///< Its path extractor.
    };

    /// The grammars that carry a dependency record. A grammar absent from this
    /// table names nothing to check — which is the whole content of the decision
    /// for Grammar::MsvcDiagnostics: a diagnostic quotes a path, it does not
    /// declare a dependency, and requiring the file a warning happened to mention
    /// would discard hits over code that merely compiled with a warning.
    ///
    /// Both extractors are the ones direct mode already uses to build a manifest,
    /// so a path is recognized here exactly as it is there.
    constexpr std::array<DependencyGrammar, 2> DependencyGrammars { {
        { .grammar = PathCanon::Grammar::GccDepfile, .extract = &ParseDepFilePaths },
        { .grammar = PathCanon::Grammar::ShowIncludes, .extract = &ParseIncludePaths },
    } };

    /// Whether this machine is answerable for `path` existing. See the header for
    /// why each exclusion is load-bearing; the order is not incidental.
    /// @param path   An extracted dependency path.
    /// @param layout This machine's roots.
    /// @return True when the path must be present for the replay to be truthful.
    [[nodiscard]] bool IsCheckable(std::string_view path, PathCanon::Layout const& layout)
    {
        if (path.empty())
            return false;
        // A surviving canonical token means LocalizeRegion left this span alone.
        if (path.front() == '<')
            return false;
        // Relative first: IsToolchainHeader reports every relative path as outside
        // the roots, so asking it first would silently skip all of them.
        if (!PathCanon::IsAbsoluteForLayout(path, layout))
            return true;
        return !IsToolchainHeader(path, layout);
    }
} // namespace

std::vector<std::string> ReplayedDependencyPaths(std::span<TextRegion const> localizedRegions,
                                                 PathCanon::Layout const& layout)
{
    std::vector<std::string> paths;
    std::unordered_set<std::string> seen;

    for (auto const& region: localizedRegions)
    {
        // The match is consumed through the loop rather than bound to a named
        // iterator, for the portability reason CmdLine.cpp's ClassifyCompiler
        // records: std::array's iterator is a raw pointer on libstdc++/libc++ and
        // a class type on MSVC, and no single spelling satisfies both plus
        // clang-tidy.
        for (DependencyGrammar const& entry: DependencyGrammars)
        {
            if (entry.grammar != region.grammar)
                continue;
            // /showIncludes repeats a header once per inclusion site — hundreds of
            // notes for a few dozen distinct files — so deduplicate before probing.
            for (auto& candidate: entry.extract(region.bytes))
            {
                if (!IsCheckable(candidate, layout))
                    continue;
                if (seen.insert(candidate).second)
                    paths.push_back(std::move(candidate));
            }
        }
    }
    return paths;
}

std::optional<std::string> MissingReplayedDependency(std::span<TextRegion const> localizedRegions,
                                                     PathCanon::Layout const& layout,
                                                     std::filesystem::path const& workingDirectory)
{
    for (auto& path: ReplayedDependencyPaths(localizedRegions, layout))
    {
        std::error_code ec;
        // operator/ replaces the left operand when the right is absolute, so this
        // one expression covers both kinds. The error_code overload is not optional:
        // the throwing one would abort a launcher whose whole contract is that a
        // cache problem never breaks a build.
        if (!std::filesystem::exists(workingDirectory / std::filesystem::path { path }, ec) && !ec)
            return std::move(path);
    }
    return std::nullopt;
}

} // namespace FastCache::Cc
