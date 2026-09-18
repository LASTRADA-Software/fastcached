// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/MetricsCatalog.hpp>
#include <FastCache/Metrics/StatsReading.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file CounterAttribution_test.cpp
/// `CounterSoleWriterTable` against the tree it describes.
///
/// ## What this is for
///
/// The attribution decides whether a figure is REPORTED, never whether it is correct, and the
/// two directions of error are not symmetric. A row attributed too WIDELY renders a plausible
/// zero -- the cost this tree paid for every row before #1484, and one somebody has already
/// been surprised by. A row attributed too NARROWLY renders `-`, which reads as *this process
/// does not do that*, and nobody re-checks it. **Only the second is silent**, so it is the one
/// a check has to be built around, and #1501's acceptance says so in as many words.
///
/// ## Why this is not a scan for `Increment(Counter::X)`
///
/// Such a scan finds 39 of 148 rows. The other 109 are written through a table -- a
/// `SurfaceRefusal` row spent by `Refuse(row)`, a `LeaseToken` outcome row, or a classifier
/// returning the row for its caller to spend -- so the counter's name appears at no call site
/// at all. A writable set derived that way would have rendered all 109 absent, which is the
/// same defect as the bug it was meant to fix, nearly four times over.
///
/// ## How the check avoids having to tell a write from a read
///
/// Distinguishing the two needs all four write spellings plus the read ones, and the list grew
/// from two to five while this was being written. Two properties do the same work without it:
///
/// 1. **Every production file naming `Counter::` is classified** -- it is in `WriterFiles` with
///    its surface, or in `ReadsOnlyFiles` with a reason. A new writer in an unlisted file then
///    fails the build, which is the direction that would otherwise be silent.
/// 2. **A classified writer file may only name counters attributed to its surface.** That is
///    exactly "attributed too narrowly", and it needs no notion of what a write looks like.
///
/// A file that only READS a counter is on the second list, so property 2 never asks about it.

using namespace FastCache;

namespace
{

/// A production file that writes counters, and the surface it belongs to.
struct SurfaceWriterFile
{
    MetricsSurface surface; ///< What this file's counters are attributed to.
    std::string_view path;  ///< Repository-relative, forward slashes.
};

/// Every file that writes a catalogue counter, with the surface it is.
///
/// **Derived by reading, one row per file the scan found writing a counter.** A file arriving
/// with no row is a FAILURE below rather than a default, because a default is how silence comes
/// to read as coverage -- which is the whole subject of this ticket.
constexpr std::array WriterFiles {
    SurfaceWriterFile { .surface = MetricsSurface::CacheAcceptPath, .path = "src/FastCache/Server/ReactorServerLoop.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CacheAcceptPath, .path = "src/FastCache/Server/Server.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CacheCompileSurface,
                        .path = "src/FastCache/Protocol/CompileCacheHandler.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CacheStorage, .path = "src/FastCache/Cache/CacheEngine.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CacheStorage, .path = "src/FastCache/Cache/ExpiryReaper.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CacheStorage, .path = "src/FastCache/Cache/ReclaimLog.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileScheduler, .path = "src/FastCache/Distributed/FleetView.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileScheduler,
                        .path = "src/FastCache/Distributed/SchedulerProtocol.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileScheduler,
                        .path = "src/FastCache/Distributed/SchedulerService.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/FastCache/Distributed/LeaseToken.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/FastCache/Distributed/RosterTrust.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/apps/fastcache-cc/CodecEnvelope.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/apps/fastcache-cc/WorkerProtocol.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker,
                        .path = "src/apps/fastcache-compile-node/CompileCapacity.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker,
                        .path = "src/apps/fastcache-compile-node/CompileResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker,
                        .path = "src/apps/fastcache-compile-node/CompileResponder.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/apps/fastcache-compile-node/NodeRoster.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::CompileWorker, .path = "src/apps/fastcache-compile-node/WorkerTier.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::ConsensusPeerWire,
                        .path = "src/FastCache/Consensus/RaftPeerRefusals.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::LiveStats, .path = "src/FastCache/Protocol/LiveStream.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::LiveStats,
                        .path = "src/apps/fastcache-compile-node/LiveStatsResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeCacheTier, .path = "src/apps/fastcache-compile-node/CacheProxy.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeCacheTier, .path = "src/apps/fastcache-compile-node/LocalCache.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeEnrollment,
                        .path = "src/apps/fastcache-compile-node/EnrollmentResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/FleetTextResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/FrameEndpoint.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/MembershipGate.hpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/NodeProofResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/NodeStatusResponder.cpp" },
    SurfaceWriterFile { .surface = MetricsSurface::NodeFrameEndpoint,
                        .path = "src/apps/fastcache-compile-node/Responders.hpp" },
};

/// A production file that names a counter and writes none.
struct ReadsOnlyFile
{
    std::string_view path; ///< Repository-relative, forward slashes.
    std::string_view why;  ///< What it does with the name instead.
};

/// Files that name a counter only to READ or RENDER it.
///
/// The reason column is the point. Without it this list and `WriterFiles` would differ only by
/// which one somebody happened to put a path in, and a file that quietly started writing would
/// be indistinguishable from one that was always a reader.
constexpr std::array ReadsOnlyFiles {
    ReadsOnlyFile { .path = "src/FastCache/Distributed/FleetView.cpp",
                    .why = "reads DispatchWorkerRegistrations for the fleet page and looks a lease outcome up in "
                           "LeaseOutcomeTable; the rows themselves are declared in FleetView.hpp" },
    ReadsOnlyFile { .path = "src/apps/fastcache-cli/DashboardPanels.cpp",
                    .why = "names counters in CounterField<> and RefusalsPerMinute<> to render a reading captured "
                           "elsewhere; fastcache-cli writes no counter at all" },
    ReadsOnlyFile { .path = "src/apps/fastcache-compile-node/AdminEndpoint.cpp",
                    .why = "Read()s NodeCacheHits and NodeCacheMisses into the fleet sample; LocalCache.cpp writes them" },
    ReadsOnlyFile { .path = "src/apps/fastcache-compile-node/CacheTier.cpp",
                    .why = "Read()s NodeCacheHits and NodeCacheMisses into its own report; LocalCache.cpp writes them" },
};

/// The files that DECLARE the vocabulary rather than using it.
///
/// `IMetricsSink.hpp` holds the enum, `MetricsCatalog.hpp` a row per counter and
/// `StatsReading.hpp` the attribution itself -- so each names every counter or nearly, and
/// classifying them as writers or readers would be answering a question they do not ask. Named
/// here rather than filtered by directory, because `Metrics/` also holds `PrometheusFormatter`
/// and this test, and a directory rule would swallow a real writer arriving beside them.
constexpr std::array DeclaringFiles {
    std::string_view { "src/FastCache/Metrics/IMetricsSink.hpp" },
    std::string_view { "src/FastCache/Metrics/MetricsCatalog.hpp" },
    std::string_view { "src/FastCache/Metrics/StatsReading.hpp" },
};

/// Headers reached only from tests, which a `*_test.cpp` filter does not catch.
///
/// **One row, and it was found by an include closure rather than by reading.**
/// `fastcache-cli/LiveSourceRig.hpp` is a fixture included from two `_test.cpp` files and names
/// a counter, so the simple filter admits it and it would have to be classified as a writer --
/// crediting `fastcache-cli`, which writes nothing, with writing one.
constexpr std::array TestOnlyHeaders {
    std::string_view { "src/apps/fastcache-cli/LiveSourceRig.hpp" },
};

/// The repository root this build was configured from.
/// @return The root path.
[[nodiscard]] std::filesystem::path RepositoryRoot()
{
    return std::filesystem::path { FASTCACHED_SOURCE_DIR };
}

/// @p path relative to the repository root, with forward slashes.
/// @param path An absolute path inside the repository.
/// @return The relative spelling the tables use.
[[nodiscard]] std::string RelativeSpelling(std::filesystem::path const& path)
{
    auto relative = std::filesystem::relative(path, RepositoryRoot()).generic_string();
    return relative;
}

/// Read @p path whole.
/// @param path The file to read.
/// @return Its bytes as text.
[[nodiscard]] std::string ReadWhole(std::filesystem::path const& path)
{
    std::ifstream input { path, std::ios::binary };
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/// Whether @p c can appear inside a C++ identifier.
/// @param c The character to classify.
/// @return True for a letter, a digit or an underscore.
[[nodiscard]] constexpr bool IsIdentifierChar(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/// @p text split on newlines, without copying it.
///
/// A helper rather than `std::getline` over a `std::istringstream` at each of the two sites that
/// walk lines: that spelling is a C-style `for` (an init, a condition and no increment), which
/// `ctest -R test-loops` refuses anywhere under `src/` -- correctly, since a new one is to be
/// converted rather than backlogged. Not `views::split` either: `std::string_view`'s range
/// constructor is C++23 (P1989) and this file must build on the AppleClang that has no
/// `ranges::iota`, so the safe spelling is one `while` here and a range-`for` at both callers.
///
/// @param text The text to split; must outlive the result, which borrows from it.
/// @return One view per line, the terminating newline excluded.
[[nodiscard]] std::vector<std::string_view> LinesOf(std::string_view text)
{
    std::vector<std::string_view> out;
    auto rest = text;
    while (!rest.empty())
    {
        auto const at = rest.find('\n');
        if (at == std::string_view::npos)
        {
            out.push_back(rest);
            break;
        }
        out.push_back(rest.substr(0, at));
        rest.remove_prefix(at + 1);
    }
    return out;
}

/// The `Counter` enumerators, in declaration order, as they are SPELLED.
///
/// Parsed out of the header rather than derived from the catalogue, because a catalogue row
/// carries the Prometheus name and this check has to match C++ source text. The parse is tied
/// to the build by the case that asserts the count equals `EnumeratorCount`, so a parse that
/// silently stopped early is a failure rather than a smaller number.
///
/// @return Every enumerator but `Last`.
[[nodiscard]] std::vector<std::string> CounterSpellings()
{
    auto const text = ReadWhole(RepositoryRoot() / "src/FastCache/Metrics/IMetricsSink.hpp");
    auto const open = text.find("enum class Counter : std::uint8_t");
    REQUIRE(open != std::string::npos);
    auto const close = text.find("\n    };", open);
    REQUIRE(close != std::string::npos);

    auto const body = std::string_view { text }.substr(open, close - open);

    std::vector<std::string> out;
    for (auto const raw: LinesOf(body))
    {
        auto const start = raw.find_first_not_of(" \t");
        if (start == std::string_view::npos)
            continue;
        auto const line = raw.substr(start);
        if (line.starts_with("//") || line.starts_with("/*") || line.starts_with("*") || line.starts_with("enum")
            || line.starts_with("{"))
            continue;

        // A row is `Name,` or `Name = 0,`; anything else is prose this parse must not eat.
        auto const end = line.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
        if (end == std::string_view::npos || end == 0)
            continue;
        auto const name = line.substr(0, end);
        auto const rest = line.substr(end);
        if (rest.starts_with(",") || rest.starts_with(" = ") || rest.starts_with(" ="))
            if (name != "Last")
                out.emplace_back(name);
    }
    return out;
}

/// Every first-party source file under `src/`, excluding tests and test-only headers.
/// @return Absolute paths.
[[nodiscard]] std::vector<std::filesystem::path> ProductionSources()
{
    std::vector<std::filesystem::path> out;
    for (auto const& entry: std::filesystem::recursive_directory_iterator { RepositoryRoot() / "src" })
    {
        if (!entry.is_regular_file())
            continue;
        auto const& path = entry.path();
        auto const extension = path.extension().string();
        if (extension != ".cpp" && extension != ".hpp" && extension != ".h")
            continue;
        auto const relative = RelativeSpelling(path);
        if (relative.ends_with("_test.cpp") || relative.contains("/tests/"))
            continue;
        if (std::ranges::find(TestOnlyHeaders, relative) != TestOnlyHeaders.end())
            continue;
        if (std::ranges::find(DeclaringFiles, relative) != DeclaringFiles.end())
            continue;
        out.push_back(path);
    }
    return out;
}

/// Which counters @p text names, by their C++ spelling.
///
/// Matches `Counter::Name` however it is qualified -- `IMetricsSink::Counter::X` and
/// `FastCache::IMetricsSink::Counter::X` both occur, and an unqualified `Counter::X` is the
/// commonest form inside the namespace.
///
/// **Reads the identifier that FOLLOWS `Counter::` rather than searching for each known name.**
/// The first version searched per name and then checked the character after the match, because
/// `Counter::WorkerJobsRefusedNoSlot` contains `Counter::WorkerJobsRefused` and a bare substring
/// search credits the shorter row with the longer one's sites -- six enumerator names here are a
/// strict prefix of another. Reading the whole identifier removes that hazard by construction
/// instead of guarding against it, and it walks the text once rather than 148 times.
///
/// @param text The file's contents.
/// @param spellings Every enumerator name.
/// @return The names found.
[[nodiscard]] std::set<std::string> CountersNamedIn(std::string_view text, std::vector<std::string> const& spellings)
{
    constexpr auto Needle = std::string_view { "Counter::" };

    std::set<std::string> out;
    auto at = text.find(Needle);
    while (at != std::string_view::npos)
    {
        auto const start = at + Needle.size();
        auto end = start;
        while (end < text.size() && IsIdentifierChar(text[end]))
            ++end;

        auto name = std::string { text.substr(start, end - start) };
        if (std::ranges::find(spellings, name) != spellings.end())
            out.insert(std::move(name));

        at = text.find(Needle, end);
    }
    return out;
}

/// The surfaces `CounterSoleWriterTable` attributes @p counter to.
/// @param counter The row.
/// @return Its surfaces.
[[nodiscard]] std::set<MetricsSurface> AttributedSurfaces(IMetricsSink::Counter counter)
{
    std::set<MetricsSurface> out;
    for (auto const& row: CounterSoleWriterTable)
        if (row.counter == counter)
            out.insert(row.surface);
    return out;
}

/// Whether @p surfaces is too NARROW for a writer living on @p surface.
///
/// Named rather than spelled inline at the one place it is asked, so the case that proves this
/// check can fail exercises the SAME expression the check does. A control built on a different
/// expression proves that expression works -- which is how a control comes to be the one
/// instrument nobody has verified.
///
/// @param surfaces What the table attributes the counter to.
/// @param surface Where a writer of it actually lives.
/// @return True when the attribution omits that writer's surface.
[[nodiscard]] bool AttributedTooNarrowly(std::set<MetricsSurface> const& surfaces, MetricsSurface surface)
{
    return !surfaces.contains(surface);
}

} // namespace

TEST_CASE("counter-attribution: the Counter enum parses back exactly as the build sees it",
          "[metrics][hygiene][counter-attribution]")
{
    auto const spellings = CounterSpellings();

    // The positive control for every other case here, and the reason it is first: all of them
    // scan source text for these names, and a parse that stopped early would make each one pass
    // over a smaller catalogue while reporting nothing. A census returning a number nobody
    // checked is the shape this repository keeps paying for.
    CHECK(spellings.size() == EnumeratorCount<IMetricsSink::Counter>);
    CHECK(spellings.front() == "ConnectionsTotal");
    CHECK(std::ranges::find(spellings, "Last") == spellings.end());

    // And every name parsed is a name the catalogue carries, which ties the text to the table.
    CHECK(CounterTable.size() == spellings.size());
}

TEST_CASE("counter-attribution: every catalogue row is attributed to at least one surface",
          "[metrics][hygiene][counter-attribution]")
{
    // `EveryCounterIsAttributed` is a `static_assert`, so this cannot fail on a build that
    // exists. It is here because the static_assert proves the property and says nothing about
    // WHICH rows -- and a reader who wants to know what the attribution covers reads a test.
    for (auto const counter: Enumerators<IMetricsSink::Counter>())
        CHECK_FALSE(AttributedSurfaces(counter).empty());

    // One row per (counter, surface) pair, so the table is longer than the catalogue by exactly
    // the number of counters written from more than one component. Stated as a comparison
    // rather than as a literal, because a literal here is a second source of truth.
    CHECK(CounterSoleWriterTable.size() >= CounterTable.size());
}

TEST_CASE("counter-attribution: every production file naming a counter is classified",
          "[metrics][hygiene][counter-attribution]")
{
    auto const spellings = CounterSpellings();
    REQUIRE(spellings.size() == EnumeratorCount<IMetricsSink::Counter>);

    std::vector<std::string> unclassified;
    auto naming = std::size_t { 0 };
    for (auto const& path: ProductionSources())
    {
        auto const text = ReadWhole(path);
        if (!text.contains("Counter::"))
            continue;
        if (CountersNamedIn(text, spellings).empty())
            continue;
        ++naming;

        auto const relative = RelativeSpelling(path);
        auto const isWriter =
            std::ranges::any_of(WriterFiles, [&relative](auto const& row) { return row.path == relative; });
        auto const isReader =
            std::ranges::any_of(ReadsOnlyFiles, [&relative](auto const& row) { return row.path == relative; });
        if (!isWriter && !isReader)
            unclassified.push_back(relative);
    }

    // A file naming a counter and appearing on neither list is the SILENT direction: it may be
    // a new writer whose counters are attributed to somebody else's surface, and nothing about
    // the scrape would say so.
    INFO("add a WriterFiles row with its surface, or a ReadsOnlyFiles row saying what it does "
         "with the name instead");
    CHECK(unclassified.empty());
    for (auto const& path: unclassified)
        UNSCOPED_INFO("unclassified: " << path);

    // The scan must be seen to have found something before its empty result means anything.
    CHECK(naming == WriterFiles.size() + ReadsOnlyFiles.size());
}

TEST_CASE("counter-attribution: no counter is attributed more narrowly than its writers",
          "[metrics][hygiene][counter-attribution]")
{
    auto const spellings = CounterSpellings();
    REQUIRE(spellings.size() == EnumeratorCount<IMetricsSink::Counter>);

    std::map<std::string, IMetricsSink::Counter> byName;
    for (auto const index: Enumerators<IMetricsSink::Counter>())
        byName.emplace(spellings.at(static_cast<std::size_t>(index)), index);

    std::vector<std::string> tooNarrow;
    auto checked = std::size_t { 0 };
    for (auto const& row: WriterFiles)
    {
        auto const text = ReadWhole(RepositoryRoot() / row.path);
        REQUIRE_FALSE(text.empty());
        for (auto const& name: CountersNamedIn(text, spellings))
        {
            ++checked;
            if (AttributedTooNarrowly(AttributedSurfaces(byName.at(name)), row.surface))
                tooNarrow.push_back(std::string { row.path } + " writes " + name);
        }
    }

    // THE point of this file. A writer file naming a counter its surface is not attributed to
    // means a process serving that surface reports the row absent while being able to write
    // it -- a `-` where a real figure belongs, which is the failure nobody re-checks.
    CHECK(tooNarrow.empty());
    for (auto const& entry: tooNarrow)
        UNSCOPED_INFO("too narrow: " << entry);

    // Proving the check can FAIL, which the assertion above cannot do on a healthy tree: take a
    // real (file, counter) pair, remove that file's surface from the attribution, and put it
    // back through `AttributedTooNarrowly` -- the same predicate, so this settles that the
    // predicate discriminates rather than that some other expression does.
    REQUIRE(checked > 0);
    auto const& sample = WriterFiles.front();
    auto const sampleCounters = CountersNamedIn(ReadWhole(RepositoryRoot() / sample.path), spellings);
    REQUIRE_FALSE(sampleCounters.empty());
    auto const real = AttributedSurfaces(byName.at(*sampleCounters.begin()));
    CHECK_FALSE(AttributedTooNarrowly(real, sample.surface));

    auto narrowed = real;
    narrowed.erase(sample.surface);
    CHECK(AttributedTooNarrowly(narrowed, sample.surface));
}

TEST_CASE("counter-attribution: the mechanism figures quoted beside the table still hold",
          "[metrics][hygiene][counter-attribution]")
{
    auto const spellings = CounterSpellings();
    REQUIRE(spellings.size() == EnumeratorCount<IMetricsSink::Counter>);

    // Four ways a row is written, and the whole reason the figures are worth pinning: a session
    // widening the attribution by reading only `SurfaceRefusal` tables reaches 101 of the 109
    // and leaves eleven rows looking unwritten. The text before the name on its own line
    // decides -- measured against a multi-line window, which agrees exactly.
    std::set<std::string> incremented;
    std::set<std::string> refusalRow;
    std::set<std::string> outcomeRow;
    std::set<std::string> returned;

    for (auto const& path: ProductionSources())
    {
        auto const text = ReadWhole(path);
        if (!text.contains("Counter::"))
            continue;
        for (auto const raw: LinesOf(text))
        {
            auto const start = raw.find_first_not_of(" \t");
            if (start == std::string_view::npos || raw.substr(start).starts_with("//"))
                continue;

            constexpr auto Needle = std::string_view { "Counter::" };
            auto at = raw.find(Needle);
            while (at != std::string_view::npos)
            {
                auto end = at + Needle.size();
                while (end < raw.size() && IsIdentifierChar(raw[end]))
                    ++end;

                auto const name = std::string { raw.substr(at + Needle.size(), end - at - Needle.size()) };
                if (std::ranges::find(spellings, name) == spellings.end())
                    continue;

                // The text before `Counter::` still carries the qualification, and all three
                // spellings occur -- bare, `IMetricsSink::`, and `FastCache::IMetricsSink::`.
                // Testing the raw prefix for `Increment(` therefore matched NOTHING and the
                // figure came back 0, which the pinned comparison caught rather than the eye.
                auto prefix = raw.substr(0, at);
                for (auto const qualifier: { std::string_view { "IMetricsSink::" }, std::string_view { "FastCache::" } })
                    while (prefix.ends_with(qualifier))
                        prefix.remove_suffix(qualifier.size());

                if (prefix.ends_with("Increment(") || prefix.ends_with("Increment( "))
                    incremented.insert(name);
                if (prefix.ends_with(".counter = "))
                    refusalRow.insert(name);
                if (prefix.ends_with(".workerCounter = "))
                    outcomeRow.insert(name);
                if (prefix.ends_with("return "))
                    returned.insert(name);

                at = raw.find(Needle, end);
            }
        }
    }

    // Pinned rather than pointed at: these describe this tree at one instant, and a figure that
    // tracked its own subject would silently re-attribute a real measurement to conditions it
    // was never taken under. Drift is a red build, which is what the previous "106 of 144" --
    // a sentence with nothing watching it -- did not get.
    CHECK(incremented.size() == 42);
    CHECK(refusalRow.size() == 111);
    CHECK(outcomeRow.size() == 10);
    CHECK(returned.size() == 4);

    std::set<std::string> anyWriter;
    for (auto const* each: { &incremented, &refusalRow, &outcomeRow, &returned })
        anyWriter.insert(each->begin(), each->end());

    // No catalogue row is written by none of the four. The check for that is the whole
    // catalogue, not a count: a row nobody writes is a row whose surface was guessed.
    CHECK(anyWriter.size() == spellings.size());
    CHECK(spellings.size() - incremented.size() == 121);

    // 111 rows have a refusal row; 110 of them have no increment site. Two figures one apart
    // measuring different things is how a census gets quoted wrong -- the first draft of the
    // comment beside `CounterSoleWriterTable` said 101 for both -- so the REACH of a
    // SurfaceRefusal-only reading is asserted separately from the row count.
    std::set<std::string> reachedByRefusalRowsAlone;
    for (auto const& name: refusalRow)
        if (!incremented.contains(name))
            reachedByRefusalRowsAlone.insert(name);
    CHECK(reachedByRefusalRowsAlone.size() == 110);

    // And four rows are written two ways, which is why the column sums to 167 over 163 rows.
    CHECK(incremented.size() + refusalRow.size() + outcomeRow.size() + returned.size() == spellings.size() + 4);
}

TEST_CASE("counter-attribution: a name that prefixes another is not credited with its mentions",
          "[metrics][hygiene][counter-attribution]")
{
    auto const spellings = CounterSpellings();
    REQUIRE(spellings.size() == EnumeratorCount<IMetricsSink::Counter>);

    // THE CONTROL, and the reason this case exists rather than trusting the boundary check to
    // be obviously right: the hazard is only real if some enumerator name is a strict prefix of
    // another, so that is measured rather than assumed. Six pairs today --
    // `ConnectionsTotal`/`ConnectionsTotalTls`, `DispatchLeasesReleased`/`...Late` and four more
    // -- which makes a bare substring search credit the shorter row with the longer one's sites
    // and attribute counters to surfaces that do not write them.
    std::vector<std::pair<std::string, std::string>> prefixPairs;
    for (auto const& shorter: spellings)
        for (auto const& longer: spellings)
            if (shorter != longer && longer.starts_with(shorter))
                prefixPairs.emplace_back(shorter, longer);
    REQUIRE_FALSE(prefixPairs.empty());

    for (auto const& [shorter, longer]: prefixPairs)
    {
        INFO(shorter << " is a prefix of " << longer);

        // The longer name alone must name the longer row and NOT the shorter one.
        auto const onlyLonger = CountersNamedIn("x = Counter::" + longer + ";", spellings);
        CHECK(onlyLonger.contains(longer));
        CHECK_FALSE(onlyLonger.contains(shorter));

        // And the shorter name alone still reads, so the boundary check has not gone too far --
        // the direction a fix for the above overshoots into.
        auto const onlyShorter = CountersNamedIn("x = Counter::" + shorter + ";", spellings);
        CHECK(onlyShorter.contains(shorter));
        CHECK_FALSE(onlyShorter.contains(longer));
    }
}
