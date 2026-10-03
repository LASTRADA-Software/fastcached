// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/ServiceStatusPlan.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <tests/DaemonHostFakes.hpp>

using namespace FastCache;

TEST_CASE("A refused service start is reported stopped with its exit code and never running", "[platform][service][refusal]")
{
    // What `sc query` shows for a service whose configuration was refused: STOPPED, WIN32_EXIT_CODE
    // 1066 (the reason is in the other field) and SERVICE_EXIT_CODE the process's own code. Never a
    // RUNNING first, or `sc start` answers success for a service that is already stopping.
    auto const& refused = ServiceHostStartTable[static_cast<std::size_t>(ServiceHostStart::Refused)];
    CHECK(refused.beforeStop.empty());
    CHECK(ServiceExitFor(2) == ServiceExit { .win32ExitCode = ServiceSpecificError, .serviceSpecificExitCode = 2 });
    CHECK(ServiceExitFor(1) == ServiceExit { .win32ExitCode = ServiceSpecificError, .serviceSpecificExitCode = 1 });
    CHECK(ServiceSpecificError == 1066);

    // The control: a start that serves says starting, then running, before its body -- and a clean
    // stop reads as one, so no restart policy fires on a service an operator asked to stop.
    auto const& serving = ServiceHostStartTable[static_cast<std::size_t>(ServiceHostStart::Serving)];
    auto const states = serving.beforeStop | std::views::transform(&ServiceStateReport::state);
    CHECK(std::ranges::equal(states, std::array { ServiceState::StartPending, ServiceState::Running }));
    CHECK(ServiceExitFor(0) == ServiceExit { .win32ExitCode = 0, .serviceSpecificExitCode = 0 });
}

TEST_CASE("A start refused through its host is said first and then reported by the host", "[platform][service][refusal]")
{
    // The host answers with a code of its own, so a caller that returned its own constant without
    // asking the host would be visible here rather than agreeing by coincidence.
    Testing::RecordingDaemonHost host { 77 };
    CapturingLogger logger;

    CHECK(RefuseStart(host, logger, "--discovery needs --listen-raft", 2) == 77);
    CHECK(host.Refusals() == std::vector<int> { 2 });
    CHECK(host.Runs() == 0);
    auto const records = logger.Snapshot();
    REQUIRE(records.size() == 1);
    CHECK(records.front().level == LogLevel::Error);
    CHECK(records.front().message == "--discovery needs --listen-raft");
}

TEST_CASE("The foreground host reports a refusal as its exit code and runs nothing", "[platform][service][refusal]")
{
    // Right for two of the three hosts: in the foreground the exit code IS the report, and a POSIX
    // daemon has not forked by the time a refusal is decided.
    ForegroundHost host;
    CHECK(host.Refuse(2) == 2);
}

namespace
{

/// One daemonizing binary, and the stretch of its `main` where a refusal has to reach the service.
struct RefusalRegion
{
    std::string_view file;   ///< Path under the repository root.
    std::string_view from;   ///< Where the region opens: what a running service reports starts here.
    std::string_view to;     ///< Where it closes: the host is entered.
    std::string_view bare;   ///< A refusal that returns without the host, spelled as this file spells it.
    std::string_view coded;  ///< The same refusal spelled through the stage table (`ExitCodeFor`), still bare.
    std::string_view routed; ///< The call a refusal in the region goes through instead.
};

/// Every daemonizing `main`, and how each one spells a refusal.
///
/// A file, an anchor or a routed call that is not found is a FAILURE, never a skip: a scan matching
/// nothing is the failure a consistency check must not have.
constexpr std::array Regions {
    RefusalRegion { .file = "src/apps/fastcache-compile-node/main.cpp",
                    .from = "ParseNodeCommandLine(argvSpan",
                    .to = "host->Run(",
                    .bare = "return EXIT_FAILURE;",
                    .coded = "return ExitCodeFor(",
                    .routed = "RefuseStart(" },
    RefusalRegion { .file = "src/apps/fastcached/main.cpp",
                    .from = "ParseCliInto(args, cli)",
                    .to = "host->Run(",
                    .bare = "return EXIT_FAILURE;",
                    .coded = "return FastCache::ExitCodeFor(",
                    .routed = "RefuseUnderService(" },
};

/// Whether every field of @p region is spelled: an empty needle is found everywhere and nowhere at
/// once, which turns the bare count into a vacuous pass and the walk below into an endless one.
/// @param region A row of `Regions`.
/// @return Whether no field is empty.
[[nodiscard]] constexpr bool FullySpelled(RefusalRegion const& region)
{
    return !region.file.empty() && !region.from.empty() && !region.to.empty() && !region.bare.empty()
           && !region.coded.empty() && !region.routed.empty();
}
static_assert(std::ranges::all_of(Regions, FullySpelled), "every Regions row names all six of its fields");

/// @p text with whole-line `//` comments removed: a comment is not a statement (#723).
/// @param text A source file.
/// @return The same text, comment-only lines blanked.
[[nodiscard]] std::string WithoutCommentLines(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (auto const line: std::views::split(text, '\n'))
    {
        std::string_view const view { line.begin(), line.end() };
        auto const first = view.find_first_not_of(" \t");
        if (first == std::string_view::npos || !view.substr(first).starts_with("//"))
            out.append(view);
        out.push_back('\n');
    }
    return out;
}

/// How often @p needle occurs in @p haystack.
/// @param haystack The text searched.
/// @param needle What is counted; an empty one counts nothing rather than looping forever.
/// @return The number of non-overlapping occurrences.
[[nodiscard]] std::size_t Occurrences(std::string_view haystack, std::string_view needle)
{
    if (needle.empty())
        return 0;
    std::size_t count = 0;
    auto at = haystack.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = haystack.find(needle, at + needle.size());
    }
    return count;
}

/// What the scan below counts in one file's text.
struct RegionCounts
{
    std::size_t bare = 0;   ///< Refusals returning without the host.
    std::size_t routed = 0; ///< Refusals routed through it.
};

/// Count @p region's refusals in @p source, comment lines excluded.
/// @param source The file's text.
/// @param region What to look for, and where.
/// @return The counts; the case has failed when either anchor is missing.
[[nodiscard]] RegionCounts CountRegion(std::string_view source, RefusalRegion const& region)
{
    auto const text = WithoutCommentLines(source);
    auto const from = text.find(region.from);
    REQUIRE(from != std::string::npos);
    auto const to = text.find(region.to, from);
    REQUIRE(to != std::string::npos);
    auto const body = std::string_view { text }.substr(from, to - from);
    return RegionCounts { .bare = Occurrences(body, region.bare) + Occurrences(body, region.coded),
                          .routed = Occurrences(body, region.routed) };
}

/// @p source with @p line inserted on the line after @p region's opening anchor.
/// @param source The file's text.
/// @param region Whose anchor.
/// @param line The line to plant, without its newline.
/// @return The planted text; the case has failed when the anchor is missing.
[[nodiscard]] std::string PlantAfterAnchor(std::string_view source, RefusalRegion const& region, std::string_view line)
{
    auto const anchor = source.find(region.from);
    REQUIRE(anchor != std::string_view::npos);
    auto const lineEnd = source.find('\n', anchor);
    REQUIRE(lineEnd != std::string_view::npos);
    std::string planted { source.substr(0, lineEnd + 1) };
    planted.append(line);
    planted.push_back('\n');
    planted.append(source.substr(lineEnd + 1));
    return planted;
}

} // namespace

TEST_CASE("No daemonizing main refuses a start without telling its service host", "[platform][service][refusal]")
{
    // Every one of these refusals used to `return` from `main` before the SCM host was entered, so a
    // service refused by name into the event log and the SCM, having never been connected to,
    // reported error 1053 -- "did not respond in a timely fashion" -- to whoever ran `sc start`.
    // `main` is in no test target, so what asserts its wiring is this.
    for (auto const& region: Regions)
    {
        INFO(region.file);
        auto const path = std::filesystem::path { FASTCACHED_SOURCE_DIR } / region.file;
        REQUIRE(std::filesystem::exists(path));
        // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
        std::ifstream in { path, std::ios::binary };
        REQUIRE(in);
        std::ostringstream contents;
        contents << in.rdbuf();
        auto const source = std::move(contents).str();

        auto const counts = CountRegion(source, region);
        CHECK(counts.bare == 0);
        // And the routed call is THERE: without it the check above passes a region that refuses
        // nothing at all, which is the tree an anchor gone stale would present.
        CHECK(counts.routed > 0);

        // The planted positive: a bare refusal inside the region IS seen, on this very file -- so a
        // zero above is a finding and not a needle that can match nothing here. And its control: the
        // same line as a comment is not a statement, and is not counted.
        // Both spellings: a refusal returning its stage's code is as bare as one returning a literal.
        for (auto const spelling: { region.bare, region.coded })
        {
            INFO(spelling);
            auto const bareLine = std::string { "    " } + std::string { spelling };
            CHECK(CountRegion(PlantAfterAnchor(source, region, bareLine), region).bare == 1);
            CHECK(CountRegion(PlantAfterAnchor(source, region, "    // " + std::string { spelling }), region).bare == 0);
        }
    }
}
