// SPDX-License-Identifier: Apache-2.0
#include "MarkerDiscovery.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace FastCache::Cc
{

namespace
{
    /// Blanks, as every trim in this file spells them. A note's depth padding is
    /// spaces; a tab is admitted because a line this rule refuses must be refused for
    /// the right reason.
    constexpr std::string_view Blanks = " \t";

    /// One path character, folded for comparison: ASCII-lowered, separators unified.
    ///
    /// More permissive than a byte compare, and stated as such on
    /// `MarkerFromProbeOutput`: this runs on the MSVC family only, where a driver
    /// legitimately echoes back a spelling of a path that differs from the one it was
    /// handed.
    /// @param c The character.
    /// @return Its folded form.
    [[nodiscard]] constexpr char FoldPathChar(char c) noexcept
    {
        if (c == '\\')
            return '/';
        return PathCanon::AsciiLower(c);
    }

    /// Whether @p line ends with @p path, comparing paths as paths.
    /// @param line The candidate line, already stripped of terminators and trailing blanks.
    /// @param path The path the probe TU includes.
    /// @return True when the line's tail is that path.
    [[nodiscard]] bool EndsWithPath(std::string_view line, std::string_view path) noexcept
    {
        if (path.empty() || line.size() < path.size())
            return false;
        auto const tail = line.substr(line.size() - path.size());
        for (auto const i: std::views::iota(std::size_t { 0 }, path.size()))
            if (FoldPathChar(tail[i]) != FoldPathChar(path[i]))
                return false;
        return true;
    }

    /// Strip a trailing `\r`, so a note is recognised on either line ending.
    /// @param line One line.
    /// @return It, without a carriage return.
    [[nodiscard]] std::string_view WithoutCarriageReturn(std::string_view line) noexcept
    {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        return line;
    }
} // namespace

std::optional<std::string> MarkerFromProbeOutput(std::string_view probeOutput, std::string_view knownPath)
{
    if (knownPath.empty())
        return std::nullopt;

    std::optional<std::string> learned;
    std::size_t offset = 0;
    while (offset < probeOutput.size())
    {
        auto lineEnd = probeOutput.find('\n', offset);
        if (lineEnd == std::string_view::npos)
            lineEnd = probeOutput.size();
        auto const line = WithoutCarriageReturn(probeOutput.substr(offset, lineEnd - offset));
        offset = lineEnd + 1;

        // The anchor, before anything else is asked. A note begins at column zero and
        // nothing may precede it -- the rule every reader in this tree shares (#1270)
        // -- so a candidate with leading blanks is refused rather than trimmed. Were it
        // trimmed, a discovery on an indented line would learn a prefix no reader could
        // then match.
        if (line.empty() || Blanks.find(line.front()) != std::string_view::npos)
            continue;

        auto const trimmedEnd = line.find_last_not_of(Blanks);
        if (trimmedEnd == std::string_view::npos)
            continue;
        auto const body = line.substr(0, trimmedEnd + 1);
        if (!EndsWithPath(body, knownPath))
            continue;

        // Everything in front of the path, with the DEPTH PADDING trimmed off the
        // right. `cl` pads between marker and path by one blank per level of inclusion
        // depth, so keeping them would make the learned prefix depend on which note it
        // was read from -- and `PathCanon::IncludeNoteMarker` carries no trailing space
        // for exactly that reason.
        auto candidate = body.substr(0, body.size() - knownPath.size());
        auto const markerEnd = candidate.find_last_not_of(Blanks);
        if (markerEnd == std::string_view::npos)
            continue; // A bare path line names no prefix at all.
        candidate = candidate.substr(0, markerEnd + 1);

        if (!learned.has_value())
        {
            learned = std::string { candidate };
            continue;
        }
        // Two candidates disagreeing means the rule matched something that is not a
        // note. Saying nothing leaves the caller on the English default, where it was;
        // answering with either would re-spell a prefix no consumer looks for.
        if (*learned != candidate)
            return std::nullopt;
    }
    return learned;
}

ResolvedMarker ResolveIncludeNoteMarker(std::string_view operatorNamed, IMarkerDiscovery* discovery)
{
    // Walked rather than branched, so the precedence lives in the table's order and a
    // fourth source is a row. Every arm is spelled -- there is no `default:` -- so an
    // added enumerator is a build failure here.
    for (auto const& row: MarkerSourceTable)
    {
        switch (row.source)
        {
            case MarkerSource::Operator:
                if (!operatorNamed.empty())
                    return { .marker = std::string { operatorNamed }, .source = row.source };
                break;
            case MarkerSource::Discovered:
                if (discovery != nullptr)
                    if (auto found = discovery->Discover(); found.has_value() && !found->empty())
                        return { .marker = std::move(*found), .source = row.source };
                break;
            case MarkerSource::Default:
                return { .marker = std::string { PathCanon::IncludeNoteMarker }, .source = row.source };
            case MarkerSource::Last:
                break;
        }
    }
    // Unreachable while the `Default` row exists, and spelled rather than asserted so
    // the function is total: a table edited down to nothing would answer the default
    // instead of falling off the end.
    return { .marker = std::string { PathCanon::IncludeNoteMarker }, .source = MarkerSource::Default };
}

namespace
{
    /// Where the probe's two files go, and what they are called.
    ///
    /// Under `temp_directory_path()` rather than beside the object, because the probe
    /// must not leave anything in a build tree the build system is watching.
    struct ProbeFiles
    {
        std::filesystem::path directory; ///< Removed once the probe has answered.
        std::filesystem::path header;    ///< What the note will name.
        std::filesystem::path source;    ///< The translation unit handed to the compiler.
    };

    /// How many names to try before giving up on an unused one. A bound rather than a
    /// loop that cannot end: every failure here costs the caller the English default,
    /// which is where it was.
    constexpr int ProbeDirectoryAttempts = 64;

    /// Write the probe TU and its header.
    ///
    /// The directory is **claimed rather than named**: `create_directory` answers true
    /// only when THIS call made it, so the filesystem settles uniqueness instead of a
    /// name predicting it. Real builds run many launchers at once and
    /// `catch_discover_tests` gives every case its own process, so a predicted name is
    /// two probes writing one header -- and the obvious entropy source is worse than a
    /// counter here, `std::random_device` having been measured answering zero for most
    /// draws on this project's own Windows host (#1507).
    ///
    /// @return The paths, or nullopt when anything could not be claimed or written.
    [[nodiscard]] std::optional<ProbeFiles> WriteProbeFiles()
    {
        std::error_code ec;
        auto const base = std::filesystem::temp_directory_path(ec);
        if (ec)
            return std::nullopt;

        ProbeFiles files;
        bool claimed = false;
        for ([[maybe_unused]] auto const attempt: std::views::iota(0, ProbeDirectoryAttempts))
        {
            static int sequence = 0;
            files.directory = base / ("fastcache-cc-marker-" + std::to_string(++sequence));
            ec.clear();
            if (std::filesystem::create_directory(files.directory, ec) && !ec)
            {
                claimed = true;
                break;
            }
        }
        if (!claimed)
            return std::nullopt;

        files.header = files.directory / "fc-marker-probe.h";
        files.source = files.directory / "fc-marker-probe.cpp";

        {
            std::ofstream header { files.header, std::ios::binary };
            // Empty but for a comment: the probe asks what a note LOOKS like, and a
            // header with declarations in it would only give the compiler more to say.
            header << "/* fastcache-cc: reads this file's name back out of a note. */\n";
            if (!header)
                return std::nullopt;
        }
        {
            std::ofstream source { files.source, std::ios::binary };
            // Quoted, and the header sits beside it, so no `-I` is needed and the
            // driver resolves it to the absolute path this function knows.
            source << "#include \"fc-marker-probe.h\"\n";
            if (!source)
                return std::nullopt;
        }
        return files;
    }

    /// Remove the probe's directory, best effort.
    /// @param files What `WriteProbeFiles` made.
    void RemoveProbeFiles(ProbeFiles const& files)
    {
        std::error_code ec;
        std::filesystem::remove_all(files.directory, ec);
    }
} // namespace

std::optional<std::string> ProbeIncludeNoteMarker(IProcessRunner& runner, std::string const& compiler)
{
    auto const files = WriteProbeFiles();
    if (!files.has_value())
        return std::nullopt;

    // `/EP` rather than `/c`: it writes no object, and the notes come out all the same.
    // `/nologo` keeps the banner line out of the stream, which matters because the
    // banner is one more line a *line ending in a known path* rule could match.
    //
    // Run through the ORDINARY runner, never `RunCaptureSplitInEnglish`. The question
    // is what the BUILD's own compiles emit, and those are not VSLANG-forced -- a probe
    // that anglicized itself would answer English on every machine and be confidently
    // wrong on the one machine this exists for.
    std::vector<std::string> const argv {
        compiler, "/nologo", "/EP", "/showIncludes", files->source.string(),
    };
    auto const run = runner.RunCaptureCombined(argv);
    RemoveProbeFiles(*files);

    // A probe that did not RUN is not one that answered nothing. `NotSpawned` is the
    // guard rather than an empty output, for the reason the toolchain probe states.
    if (run.exitCode == NotSpawned)
        return std::nullopt;

    return MarkerFromProbeOutput(run.out + run.err, files->header.string());
}

} // namespace FastCache::Cc
