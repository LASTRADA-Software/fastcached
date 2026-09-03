// SPDX-License-Identifier: Apache-2.0
#include "DirectManifest.hpp"
#include "ReplayGuard.hpp"

#include <FastCache/Platform/NarrowText.hpp>

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
        // Classified first: IsToolchainHeader reports every path outside the roots
        // as toolchain, a relative one included, so asking it first would silently
        // skip all of them.
        switch (PathCanon::AnchorForLayout(path, layout))
        {
            case PathCanon::Anchor::WorkingDirectory:
                // Checked: it resolves against the compile's working directory,
                // which is also the launcher's, so it is this machine's path.
                return true;
            case PathCanon::Anchor::DriveRelative:
                // Not checked, because the check cannot be made truthfully. `C:foo`
                // resolves against drive C's own current directory, and
                // std::filesystem::operator/ reaches neither: on a POSIX host
                // `wd / "C:foo"` is a name that exists nowhere, so every hit
                // carrying such a path would be discarded forever; on Windows it
                // either replaces the left operand or appends against the PROCESS
                // cwd. This is the policy the header already states for a path that
                // cannot be examined at all — it counts as present. See issue #65.
                //
                // Deliberately NOT deferred to the root tests, unlike the key
                // filter's matching branch: what that one needs is a portable
                // spelling, which a drive-relative root can still supply, whereas
                // what this one needs is a path to stat, which it cannot. So for a
                // drive-relative root this arm is a behaviour *change* — such a
                // path used to be probed, against a working directory that is not
                // the one it is anchored to, which discarded every hit carrying it.
                return false;
            case PathCanon::Anchor::Absolute:
                break;
        }
        // A near miss of a root is checked, and it is the one path outside the roots
        // that is. The exclusion above rests on a REASON rather than on the word
        // "outside": a path out here is toolchain or system content, which the
        // toolchain stamp covers collectively, so a machine that has a different one
        // has a different key and never sees this value at all. A root spelled
        // almost right is outside the roots and covered by nothing -- it is a
        // project header the roots failed to name -- so the stamp argument does not
        // reach it, and skipping it would replay a path naming a file that may be
        // somewhere else entirely on this machine, which is issue #53 exactly.
        //
        // This is a difference from the key filter's treatment of the same path,
        // stated here because the two questions differ: the key asks what is safe to
        // HASH (a machine-specific path, so it is dropped), and this asks what this
        // machine is answerable for EXISTING (a project header, so it is probed).
        // What they no longer differ about is which paths are outside the roots at
        // all -- one `PathCanon::IsUnderRoot` decides that for both (issue #562).
        if (IsNearMissRoot(path, layout))
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
        // Read as text FIRST, and the `error_code` overload below is no help with
        // it: this path came out of a STORED value, so its bytes were written by
        // whichever machine produced that entry -- and on a host that reads narrow
        // bytes as UTF-8 the `path` CONSTRUCTOR throws for bytes that are not,
        // before any error_code is consulted. This is the one input here that
        // arrives over the network, so it is also the one a remote entry could use
        // to take a build down.
        //
        // Unreadable counts as MISSING, which discards the hit and recompiles. That
        // is the honest answer as much as the safe one: a dependency this host
        // cannot even name is one it cannot check, and serving a hit it could not
        // check is what this guard exists to prevent.
        auto const dependency = PathFromNarrowText(path);
        if (!dependency.has_value())
            return std::move(path);

        std::error_code ec;
        // operator/ replaces the left operand when the right is absolute, so this
        // one expression covers both kinds. The error_code overload is not optional:
        // the throwing one would abort a launcher whose whole contract is that a
        // cache problem never breaks a build.
        if (!std::filesystem::exists(workingDirectory / *dependency, ec) && !ec)
            return std::move(path);
    }
    return std::nullopt;
}

} // namespace FastCache::Cc
