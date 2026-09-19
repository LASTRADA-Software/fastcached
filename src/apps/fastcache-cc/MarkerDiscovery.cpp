// SPDX-License-Identifier: Apache-2.0
#include "MarkerDiscovery.hpp"

#include <algorithm>
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
        return std::ranges::equal(line.substr(line.size() - path.size()), path, {}, FoldPathChar, FoldPathChar);
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
        if (line.empty() || Blanks.contains(line.front()))
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
    // In `MarkerSource`'s own order. A first draft walked `MarkerSourceTable` with a
    // `switch` inside the loop, which reads as data-driven and is not: the table supplies
    // labels only, every arm still had to be written, and `RowsInEnumeratorOrder` already
    // pins the table to the enum -- so the walk could never take an order different from
    // the one the arms are written in. Three guarded returns say the same thing and
    // cannot disagree with themselves.
    // `MarkerDiscoveryRequest` is handled HERE rather than at the call site, so the one
    // value that means *ask, do not tell* has one reading. It names no prefix, so the
    // `Operator` row declines it exactly as an unset variable is declined -- and a caller
    // that mapped it to empty itself would be a second place for that to drift.
    if (!operatorNamed.empty() && operatorNamed != MarkerDiscoveryRequest)
        return { .marker = std::string { operatorNamed }, .source = MarkerSource::Operator };

    // Not asked unless a probe was handed in, and an empty answer is no answer: a prefix
    // that matches every line is not one Ninja could match a note against.
    //
    // Asking and being declined is its OWN answer, not the default one. Both leave the
    // English marker in place, so nothing about the VALUE distinguishes them -- which is
    // why the distinction has to live in the source rather than be re-derived later from
    // a string that is equal in both cases. An operator here wrote
    // `FASTCACHE_MSVC_DEPS_PREFIX=auto` and is owed a different sentence from one who
    // wrote nothing at all.
    if (discovery != nullptr)
    {
        auto found = discovery->Discover();
        if (found.has_value() && !found->empty())
            return { .marker = std::move(*found), .source = MarkerSource::Discovered };
        return { .marker = std::string { PathCanon::IncludeNoteMarker }, .source = MarkerSource::DiscoveryDeclined };
    }

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

    /// Remove the probe's directory, best effort.
    /// @param files What `WriteProbeFiles` made.
    void RemoveProbeFiles(ProbeFiles const& files)
    {
        std::error_code ec;
        std::filesystem::remove_all(files.directory, ec);
    }

    /// Write one small text file, closing it before the status is believed.
    ///
    /// `close()` first, deliberately: a stream that is still open has not necessarily
    /// flushed, so `if (!stream)` on an open `ofstream` answers about the formatting
    /// rather than about the write. A full disk or a quota then reads as SUCCESS and
    /// the compiler is handed a truncated header -- which the probe would go on to
    /// misread as "this compiler will not say", the one outcome it cannot tell apart
    /// from a real answer.
    ///
    /// @param path What to write.
    /// @param contents What to put in it.
    /// @return True when the bytes reached the filesystem.
    [[nodiscard]] bool WriteTextFile(std::filesystem::path const& path, std::string_view contents)
    {
        std::ofstream out { path, std::ios::binary };
        out << contents;
        out.close();
        return static_cast<bool>(out);
    }

    /// Write the probe TU and its header.
    ///
    /// The directory is **claimed rather than named**: `create_directory` answers true
    /// only when THIS call made it, so the filesystem settles uniqueness instead of a
    /// name predicting it. Real builds run many launchers at once and
    /// `catch_discover_tests` gives every case its own process, so a predicted name is
    /// two probes writing one header -- and the obvious entropy source is worse than the
    /// attempt counter here, `std::random_device` having been measured answering zero for
    /// most draws on this project's own Windows host (#1507).
    ///
    /// **Every failure path after the claim removes the directory again**, because the
    /// claim and the attempt bound compose into a trap otherwise: a leaked
    /// `fastcache-cc-marker-<n>` is a name no later process can claim, `temp_directory_path()`
    /// is shared between processes and users, and after `ProbeDirectoryAttempts` of them
    /// the probe stops working on this host for good -- silently, since it fails open.
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
        // The ATTEMPT is the name, not a process-lifetime counter: `create_directory` is
        // what settles uniqueness, so nothing is gained by remembering how many names a
        // previous call burned -- and a mutable `static` in a launcher that may one day
        // probe off more than one thread is a data race nothing would report.
        for (auto const attempt: std::views::iota(0, ProbeDirectoryAttempts))
        {
            files.directory = base / ("fastcache-cc-marker-" + std::to_string(attempt));
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

        // Empty but for a comment: the probe asks what a note LOOKS like, and a header
        // with declarations in it would only give the compiler more to say. The TU's
        // include is quoted and the header sits beside it, so no `-I` is needed and the
        // driver resolves it to the absolute path this function knows.
        if (!WriteTextFile(files.header, "/* fastcache-cc: reads this file's name back out of a note. */\n")
            || !WriteTextFile(files.source, "#include \"fc-marker-probe.h\"\n"))
        {
            RemoveProbeFiles(files);
            return std::nullopt;
        }
        return files;
    }
} // namespace

std::optional<std::string> ProbeIncludeNoteMarker(IProcessRunner& runner,
                                                  std::string const& compiler,
                                                  DriverSpec const& driver)
{
    auto const files = WriteProbeFiles();
    if (!files.has_value())
        return std::nullopt;

    // The driver's OWN rows rather than literals of this file's. `preprocessFlags` is
    // `/EP` and carries the comment recording why it is never `/EP /P`; a second literal
    // here would sit outside that rule's reach and outside the table, where the two MSVC
    // drivers are free to differ. Preprocess rather than compile because it writes no
    // object and the notes come out all the same; `/nologo` is ours because it is about
    // this stream rather than about the driver -- the banner is one more line a *line
    // ending in a known path* rule could match.
    //
    // Run through the ORDINARY runner, never `RunCaptureSplitInEnglish`. The question
    // is what the BUILD's own compiles emit, and those are not VSLANG-forced -- a probe
    // that anglicized itself would answer English on every machine and be confidently
    // wrong on the one machine this exists for.
    std::vector<std::string> argv { compiler, "/nologo" };
    for (auto const flag: driver.preprocessFlags)
        argv.emplace_back(flag);
    for (auto const flag: driver.dependencyProbeFlags)
        argv.emplace_back(flag);
    argv.push_back(files->source.string());
    auto const run = runner.RunCaptureCombined(argv);
    RemoveProbeFiles(*files);

    // A probe that did not RUN is not one that answered nothing. `NotSpawned` is the
    // guard rather than an empty output, for the reason the toolchain probe states.
    if (run.exitCode == NotSpawned)
        return std::nullopt;

    // `out` ALONE, because `RunCaptureCombined` states that it carries everything and
    // that `err` stays empty -- both platform runners give the child the out pipe as its
    // stderr handle, so nothing ever reaches `err` here. Consulting it anyway would read
    // as a defence against something this seam excludes.
    //
    // Whoever moves this to `RunCaptureSplit` must JOIN the two with a newline rather
    // than concatenate them: `out` is not guaranteed to end in one, so `out + err` fuses
    // its last line onto the note on `err`'s first. That fused line still begins at
    // column zero, so it passes the anchor, still ends in the probe's path, and teaches
    // `<leftover>Note: including file:` as the prefix -- and the disagreement guard
    // cannot catch it, because the probe includes exactly one header and so emits
    // exactly one note.
    return MarkerFromProbeOutput(run.out, files->header.string());
}

} // namespace FastCache::Cc
