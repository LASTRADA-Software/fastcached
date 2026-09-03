// SPDX-License-Identifier: Apache-2.0
//
// Tests for the invocation log and its report renderer.
//
// The log location is resolved from the environment (XDG_STATE_HOME on POSIX,
// LOCALAPPDATA on Windows), which is the seam these tests use: each test points
// it at a throwaway directory so the developer's real statistics are never read
// or written.

#include "Stats.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include <tests/ScratchPath.hpp>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

using namespace FastCache::Cc;

namespace
{

/// Redirects the statistics log into a temporary directory for one test, and
/// restores the previous environment afterwards.
class ScopedStateDir
{
  public:
    ScopedStateDir()
    {
        // The directory is the shared helper's job, which this file's own comment
        // used to explain at length. The explanation moved with it -- three later
        // test files reintroduced exactly that bug while the fix sat here as a
        // private helper, and a fifth reintroduced it because the shared version
        // was somewhere it could not include from.
        _previous = Current();
        Set(_scratch.Path().string());
        static_cast<void>(ResetLog()); // start from a known-empty log
    }

    ~ScopedStateDir()
    {
        // Only the environment: the directory goes with `_scratch`, and it must
        // go AFTER this, which member order already guarantees.
        if (_previous.has_value())
            Set(*_previous);
        else
            Unset();
    }

    ScopedStateDir(ScopedStateDir const&) = delete;
    ScopedStateDir& operator=(ScopedStateDir const&) = delete;
    ScopedStateDir(ScopedStateDir&&) = delete;
    ScopedStateDir& operator=(ScopedStateDir&&) = delete;

  private:
    FastCache::Testing::ScratchDirectory _scratch { "fastcache-cc-test" };

#if defined(_WIN32)
    static constexpr char const* VariableName = "LOCALAPPDATA";
#else
    static constexpr char const* VariableName = "XDG_STATE_HOME";
#endif

    [[nodiscard]] static std::optional<std::string> Current()
    {
        char const* const value = std::getenv(VariableName);
        return value != nullptr ? std::optional { std::string { value } } : std::nullopt;
    }

    static void Set(std::string const& value)
    {
#if defined(_WIN32)
        static_cast<void>(::_putenv_s(VariableName, value.c_str()));
#else
        static_cast<void>(::setenv(VariableName, value.c_str(), 1));
#endif
    }

    static void Unset()
    {
#if defined(_WIN32)
        static_cast<void>(::_putenv_s(VariableName, ""));
#else
        static_cast<void>(::unsetenv(VariableName));
#endif
    }

    std::optional<std::string> _previous;
};

/// Build a record with the fields these tests care about.
[[nodiscard]] Record MakeRecord(Outcome outcome, std::string prefetchGroup, std::string source, std::uint64_t elapsedMs)
{
    Record record;
    record.outcome = outcome;
    record.prefetchGroup = std::move(prefetchGroup);
    record.source = std::move(source);
    record.elapsedMs = elapsedMs;
    return record;
}

/// Build a miss carrying one dispatch state and reason.
[[nodiscard]] Record MakeDispatchRecord(DispatchOutcome dispatch, std::string_view detail, std::string_view source)
{
    auto record = MakeRecord(Outcome::Miss, "main", std::string { source }, 900);
    record.dispatch = dispatch;
    record.dispatchDetail = detail;
    return record;
}

/// Append `count` identical such records.
///
/// Every distribution case sets up this way and they differed only in the count, the
/// state and the reason — the copy-paste-differing-by-a-constant the guidelines name
/// outright. Folded here, the assertion is the visible part of each case again.
void AppendDispatchRecords(int count,
                           DispatchOutcome dispatch,
                           std::string_view detail = {},
                           std::string_view source = "a.cpp")
{
    for (int i = 0; i < count; ++i)
        AppendRecord(MakeDispatchRecord(dispatch, detail, source));
}

/// Write one raw log line, for the shapes `AppendRecord` cannot produce: a
/// pre-upgrade line, and a line written by a build newer than this one.
void AppendRawLine(std::string_view line)
{
    std::ofstream out { LogPath(), std::ios::binary | std::ios::app };
    out << line << '\n';
}

} // namespace

TEST_CASE("ToStringView round-trips every outcome token")
{
    CHECK(ToStringView(Outcome::Hit) == "HIT");
    CHECK(ToStringView(Outcome::Miss) == "MISS");
    CHECK(ToStringView(Outcome::Uncacheable) == "UNCACHEABLE");
    CHECK(ToStringView(Outcome::Unavailable) == "UNAVAILABLE");
}

TEST_CASE("RecordingFor names what an operator reads for each dispatch status")
{
    // The bridge between the two enumerations, asserted rather than assumed.
    // `RowsInEnumeratorOrder` proves that table is TOTAL, never that it is RIGHT:
    // swap the declined and unreachable rows and every static assertion still passes
    // while an operator is sent to look at the network for a fleet that was merely
    // busy. Only a test tells those apart -- which is why the table had to leave
    // `main.cpp`, a file in no test target.
    CHECK(RecordingFor(DispatchStatus::Compiled).outcome == DispatchOutcome::Dispatched);
    CHECK(RecordingFor(DispatchStatus::Compiled).reason.empty());

    CHECK(RecordingFor(DispatchStatus::Declined).outcome == DispatchOutcome::Declined);
    CHECK(RecordingFor(DispatchStatus::Declined).reason == "the fleet declined this compile");

    CHECK(RecordingFor(DispatchStatus::Unavailable).outcome == DispatchOutcome::Unreachable);
    CHECK(RecordingFor(DispatchStatus::Unavailable).reason == "the fleet could not be reached");

    // A state and no reason: the cache axis already carries #280's sentence, and one
    // report must not print it under two headings.
    CHECK(RecordingFor(DispatchStatus::Mismatched).outcome == DispatchOutcome::Mismatched);
    CHECK(RecordingFor(DispatchStatus::Mismatched).reason.empty());
}

TEST_CASE("ToStringView names every dispatch token")
{
    // The write half; the read half is `ParseLog`, below. One table serves both,
    // because a token written in one spelling and read back in another loses the
    // whole axis silently -- every record decodes as `Unknown` and the report says
    // nothing, which is indistinguishable from a build that never dispatched.
    CHECK(ToStringView(DispatchOutcome::Unknown) == "UNKNOWN");
    CHECK(ToStringView(DispatchOutcome::NotConfigured) == "NOT_CONFIGURED");
    CHECK(ToStringView(DispatchOutcome::NotAttempted) == "NOT_ATTEMPTED");
    CHECK(ToStringView(DispatchOutcome::Refused) == "REFUSED");
    CHECK(ToStringView(DispatchOutcome::Declined) == "DECLINED");
    CHECK(ToStringView(DispatchOutcome::Unreachable) == "UNREACHABLE");
    CHECK(ToStringView(DispatchOutcome::Mismatched) == "MISMATCHED");
    CHECK(ToStringView(DispatchOutcome::Discarded) == "DISCARDED");
    CHECK(ToStringView(DispatchOutcome::Dispatched) == "DISPATCHED");
}

TEST_CASE("AppendRecord round-trips the dispatch axis through ParseLog")
{
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Miss, "main", "a.cpp", 900);
    record.dispatch = DispatchOutcome::Unreachable;
    record.dispatchDetail = "the fleet could not be reached";
    AppendRecord(record);

    auto const entries = ParseLog("");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().dispatch == DispatchOutcome::Unreachable);
    CHECK(entries.front().dispatchDetail == "the fleet could not be reached");
    // The cache axis is untouched by any of this: the daemon answered honestly and
    // the object was compiled locally and stored, so the compile really was a miss.
    CHECK(entries.front().outcome == Outcome::Miss);
}

TEST_CASE("A build whose every dispatch failed says so rather than reading as an ordinary miss rate")
{
    // The whole of #427. What a launcher pointed at a fleet it cannot reach records
    // is a run of perfectly honest MISSes -- the cache answered, the local compiler
    // ran, the object was stored. Before the dispatch axis existed that was the
    // ENTIRE record: the report showed a normal miss rate and said nothing at all
    // about distribution unless somebody happened to have FASTCACHE_VERBOSE set.
    ScopedStateDir const scoped;
    AppendDispatchRecords(5, DispatchOutcome::Unreachable, "the fleet could not be reached");

    auto const report = FormatReport("");
    CHECK(report.contains("misses       : 5")); // the cache axis still reads honestly
    CHECK(report.contains("distribution"));
    CHECK(report.contains("unreachable"));
    // Zero dispatched is PRINTED rather than dropped: a fleet that dispatched
    // nothing is exactly what this section exists to make visible, and an omitted
    // line would read as "no data" instead of as "none". Asserted through the rate,
    // which only the dispatched line can emit.
    CHECK(report.contains("0.0% of 5 asked of the fleet"));
    CHECK(report.contains("5x  the fleet could not be reached"));
}

TEST_CASE("A launcher with no scheduler reports no fleet rather than a failed one")
{
    // The `NoUpstream` guard, in a new place. An absence counted as an event made a
    // node with no shared cache report a 100% upstream failure rate; a launcher with
    // no FASTCACHE_SCHEDULER must not report a 100% dispatch failure rate. It has no
    // fleet, so the section does not exist for it.
    ScopedStateDir const scoped;
    AppendDispatchRecords(4, DispatchOutcome::NotConfigured);

    auto const report = FormatReport("");
    CHECK(report.contains("misses       : 4"));
    CHECK_FALSE(report.contains("distribution"));
    CHECK_FALSE(report.contains("asked of the fleet"));
}

TEST_CASE("A pre-upgrade log line makes no claim about the operator's fleet")
{
    // Eleven tab-separated fields: the shape written before the dispatch columns
    // existed. Decoding the absence as `NotConfigured` would turn a missing column
    // into an assertion that this machine has no fleet -- and decoding it as any
    // other state would invent a dispatch that was never recorded. `Unknown` is
    // neither, so the section stays absent.
    ScopedStateDir const scoped;
    AppendRawLine("MISS\tmain\t0\t10\ta.cpp\t\t0\t0\t0\t0\t1700000000");

    auto const entries = ParseLog("");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().dispatch == DispatchOutcome::Unknown);
    CHECK(entries.front().dispatchDetail.empty());
    CHECK_FALSE(FormatReport("").contains("distribution"));
}

TEST_CASE("A dispatch token this build does not know decodes as unknown rather than as a fleet state")
{
    // A line written by a LATER build. Guessing at it would be a claim about a fleet
    // made from a word this build cannot interpret.
    ScopedStateDir const scoped;
    AppendRawLine("MISS\tmain\t0\t10\ta.cpp\t\t0\t0\t0\t0\t1700000000\tQUARANTINED\tsomething new");

    auto const entries = ParseLog("");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().dispatch == DispatchOutcome::Unknown);
}

TEST_CASE("A dispatch reason is never ranked beside a cache reason")
{
    // Two axes, two lists. A compile can miss the cache AND fail to dispatch, and an
    // operator fixes those in two different places -- ranked together, a fleet
    // failure would appear in the list somebody reads to decide whether the daemon
    // is healthy.
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Unavailable, "main", "a.cpp", 5);
    record.detail = "connect failed";
    record.dispatch = DispatchOutcome::Declined;
    record.dispatchDetail = "the fleet declined this compile";
    AppendRecord(record);

    auto const report = FormatReport("");
    auto const cacheHeading = report.find("fall-back reasons");
    auto const fleetHeading = report.find("why distribution did not help");
    REQUIRE(cacheHeading != std::string::npos);
    REQUIRE(fleetHeading != std::string::npos);
    CHECK(report.find("connect failed") < fleetHeading);
    CHECK(report.find("the fleet declined this compile") > fleetHeading);
}

TEST_CASE("A crossed reply is ranked once, not once per axis")
{
    // #280's sentence belongs to the CACHE axis, where the rulebook puts it: the
    // outcome stays an honest MISS carrying that reason. The dispatch axis names the
    // same event with a state of its own, so it carries no reason -- printing the
    // identical sentence under two headings of one report reads as two events.
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Miss, "main", "a.cpp", 900);
    record.detail = "a worker answered about a different compile";
    record.dispatch = DispatchOutcome::Mismatched;
    AppendRecord(record);

    auto const report = FormatReport("");
    CHECK(report.contains("crossed reply"));
    // Exactly one ranking line for it, under the cache heading.
    auto const first = report.find("1x  a worker answered about a different compile");
    REQUIRE(first != std::string::npos);
    CHECK(report.find("1x  a worker answered about a different compile", first + 1) == std::string::npos);
    CHECK(first < report.find("distribution"));
}

TEST_CASE("A refusal on this machine is not reported as the fleet declining")
{
    // `Refused` never asked the fleet, so it is excluded from the denominator the
    // dispatch rate is taken over. Counted as an attempt it would blame a fleet for
    // a command line this launcher would not send in the first place.
    ScopedStateDir const scoped;
    AppendDispatchRecords(3, DispatchOutcome::Refused, "the command line is not dispatchable");
    AppendDispatchRecords(1, DispatchOutcome::Dispatched, {}, "b.cpp");

    auto const report = FormatReport("");
    CHECK(report.contains("refused here"));
    // One asked, one dispatched: the three refusals are not in the denominator.
    CHECK(report.contains("100.0% of 1 asked of the fleet"));
}

TEST_CASE("A fleet whose every result was thrown away does not headline as fully dispatched")
{
    // A worker ran the compiler and the client kept nothing. That is `Discarded` and
    // NOT `Dispatched` with an explanatory string, because the reports rate
    // `Dispatched` against what was asked: as a reason on `Dispatched` this reads
    // "100.0% of 2 asked of the fleet" -- a green headline over two compiles done
    // twice, with the contradiction demoted to a free-text tally underneath. That is
    // #427's own defect one layer in, so the state carries it.
    ScopedStateDir const scoped;
    AppendDispatchRecords(2, DispatchOutcome::Discarded, "a worker compile failed and was retried locally");

    auto const report = FormatReport("");
    CHECK(report.contains("result discarded"));
    CHECK(report.contains("0.0% of 2 asked of the fleet"));
    CHECK_FALSE(report.contains("100.0% of 2 asked of the fleet"));
    // The reason still says WHICH of the three discard causes it was.
    CHECK(report.contains("2x  a worker compile failed and was retried locally"));
}

TEST_CASE("A discarded result is still counted as having asked the fleet")
{
    // The exchange worked, so it belongs in the denominator: half the fleet's
    // answers being thrown away must read as 50%, never as 100% over the half that
    // survived.
    ScopedStateDir const scoped;
    AppendDispatchRecords(1, DispatchOutcome::Dispatched);
    AppendDispatchRecords(1, DispatchOutcome::Discarded, "the dispatched object could not be written", "b.cpp");

    CHECK(FormatReport("").contains("50.0% of 2 asked of the fleet"));
}

TEST_CASE("FormatHtmlReport carries the distribution panel only when there is a fleet")
{
    ScopedStateDir const scoped;
    AppendDispatchRecords(1, DispatchOutcome::Declined, "the fleet declined this compile");
    auto const withFleet = FormatHtmlReport("");
    CHECK(withFleet.contains(">distribution<"));
    CHECK(withFleet.contains("asked of the fleet"));
    CHECK(withFleet.contains("the fleet declined this compile"));

    REQUIRE(ResetLog());
    AppendDispatchRecords(1, DispatchOutcome::NotConfigured);
    auto const withoutFleet = FormatHtmlReport("");
    CHECK_FALSE(withoutFleet.contains(">distribution<"));
    CHECK_FALSE(withoutFleet.contains("asked of the fleet"));
}

TEST_CASE("FormatReport explains an empty log instead of printing zeroes")
{
    ScopedStateDir const scoped;
    auto const report = FormatReport("");
    CHECK(report.contains("no statistics recorded yet"));
}

TEST_CASE("FormatReport counts the hit rate over cacheable compiles only")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Hit, "main", "b.cpp", 12));
    AppendRecord(MakeRecord(Outcome::Miss, "main", "c.cpp", 200));
    // Unavailable compiles never reached the cache, so they must not dilute the
    // hit rate — they are reported separately as "CACHE NOT REACHED".
    AppendRecord(MakeRecord(Outcome::Unavailable, "main", "d.cpp", 190));

    auto const report = FormatReport("");
    CHECK(report.contains("compiles     : 4"));
    CHECK(report.contains("66.7% of 3 cacheable"));
    CHECK(report.contains("CACHE NOT REACHED"));
}

TEST_CASE("FormatReport ranks the fall-back reasons")
{
    ScopedStateDir const scoped;
    for (int i = 0; i < 3; ++i)
    {
        auto record = MakeRecord(Outcome::Unavailable, "main", "a.cpp", 5);
        record.detail = "connect failed";
        AppendRecord(record);
    }
    auto once = MakeRecord(Outcome::Unavailable, "main", "b.cpp", 5);
    once.detail = "preprocess failed";
    AppendRecord(once);

    auto const report = FormatReport("");
    CHECK(report.contains("fall-back reasons"));
    CHECK(report.contains("3x  connect failed"));
    CHECK(report.contains("1x  preprocess failed"));
}

TEST_CASE("A miss that fell back from a worker is still ranked by its reason")
{
    // The launcher's substitute for a counter, and the reason it works at all: the
    // reason tally is keyed on the DETAIL and is indifferent to the outcome.
    //
    // A dispatched compile whose reply did not belong to its request (#280) is
    // refused and compiled locally, but the cache answered honestly and still stores
    // the object -- so the outcome is a MISS with a fall-back reason on it. Recording
    // it as `Unavailable` instead would blame the cache and file the source under
    // "never cached", both untrue; and since `fastcache-cc` is one short-lived process
    // per translation unit with no metrics sink, this ranking is the only aggregate of
    // that event that exists anywhere.
    ScopedStateDir const scoped;
    for (int i = 0; i < 2; ++i)
    {
        auto record = MakeRecord(Outcome::Miss, "main", "a.cpp", 5);
        record.detail = "a worker answered about a different compile";
        AppendRecord(record);
    }

    auto const report = FormatReport("");
    CHECK(report.contains("2x  a worker answered about a different compile"));
    // Still a miss: the cache was never the thing that failed.
    CHECK(report.contains("misses"));
    CHECK_FALSE(report.contains("never cached"));
}

TEST_CASE("FormatReport restricts the fold to one prefetch group when filtered")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "alpha", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "b.cpp", 20));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "c.cpp", 30));

    auto const alpha = FormatReport("alpha");
    CHECK(alpha.contains("compiles     : 1"));

    auto const beta = FormatReport("beta");
    CHECK(beta.contains("compiles     : 2"));

    // A prefetch group with no records says so rather than reporting an empty fold.
    CHECK(FormatReport("gamma").contains("no records for prefetch group 'gamma'"));
}

TEST_CASE("FormatReport separates a direct-mode hit from a preprocessed one")
{
    ScopedStateDir const scoped;
    auto direct = MakeRecord(Outcome::Hit, "main", "a.cpp", 20);
    direct.directHit = true;
    direct.directMs = 18;
    AppendRecord(direct);

    auto const report = FormatReport("");
    // Direct hits are the cheap path; the report calls them out so a drop in
    // their share is visible rather than hidden inside the overall hit rate.
    CHECK(report.contains("via direct"));
}

TEST_CASE("FormatReport lists the translation units that never hit")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Uncacheable, "main", "volatile.cpp", 40));
    AppendRecord(MakeRecord(Outcome::Uncacheable, "main", "volatile.cpp", 41));

    auto const report = FormatReport("");
    CHECK(report.contains("never cached"));
    CHECK(report.contains("volatile.cpp"));
}

TEST_CASE("ResetLog empties the recorded statistics")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));
    REQUIRE(FormatReport("").contains("compiles     : 1"));

    CHECK(ResetLog());
    CHECK(FormatReport("").contains("no statistics recorded yet"));
}

TEST_CASE("FormatHtmlReport explains an empty log instead of an empty document")
{
    ScopedStateDir const scoped;
    auto const report = FormatHtmlReport("");
    CHECK(report.contains("no statistics recorded yet"));
    // Explicitly NOT html in the empty case: mirrors FormatReport's plain-text
    // explanation, and a caller piping this to a browser or a log sees a
    // one-line message either way, not a broken half-page.
    CHECK_FALSE(report.contains("<html"));
}

TEST_CASE("FormatHtmlReport is a self-contained HTML document")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));

    auto const report = FormatHtmlReport("");
    CHECK(report.starts_with("<!doctype html>"));
    CHECK(report.contains("<html"));
    CHECK(report.contains("</html>"));
    // No external network dependency: everything the page needs travels in
    // the file, so it opens correctly from a detached copy (attached to a CI
    // run, emailed, opened offline).
    CHECK_FALSE(report.contains("http://"));
    CHECK_FALSE(report.contains("https://"));
}

TEST_CASE("FormatHtmlReport surfaces the headline hit rate and tallies")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Hit, "main", "b.cpp", 12));
    AppendRecord(MakeRecord(Outcome::Miss, "main", "c.cpp", 200));

    auto const report = FormatHtmlReport("");
    CHECK(report.contains("66.7%")); // 2 hits of 3 cacheable
    CHECK(report.contains(">2<"));   // hits tally
    CHECK(report.contains(">1<"));   // misses tally
}

TEST_CASE("FormatHtmlReport lists every prefetch group in the comparison table")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "alpha", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "b.cpp", 20));

    auto const report = FormatHtmlReport("");
    CHECK(report.contains("alpha"));
    CHECK(report.contains("beta"));
}

TEST_CASE("FormatHtmlReport lists the fall-back reasons and never-cached files")
{
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Unavailable, "main", "a.cpp", 5);
    record.detail = "connect failed";
    AppendRecord(record);
    AppendRecord(MakeRecord(Outcome::Uncacheable, "main", "volatile.cpp", 40));

    auto const report = FormatHtmlReport("");
    CHECK(report.contains("connect failed"));
    CHECK(report.contains("volatile.cpp"));
}

TEST_CASE("FormatHtmlReport restricts the fold to one prefetch group when filtered")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "alpha", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "b.cpp", 20));

    auto const alpha = FormatHtmlReport("alpha");
    CHECK(alpha.contains("alpha"));
    CHECK_FALSE(alpha.contains(">beta<"));

    CHECK(FormatHtmlReport("gamma").contains("no records for prefetch group 'gamma'"));
}

TEST_CASE("LogPath points inside the configured state directory")
{
    ScopedStateDir const scoped;
    auto const path = LogPath();
    CHECK_FALSE(path.empty());
    CHECK(path.contains("fastcache-cc"));
}

TEST_CASE("AppendRecord round-trips the timestamp through ParseLog")
{
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Hit, "main", "a.cpp", 10);
    record.timestampUnixSeconds = 1'700'000'000;
    AppendRecord(record);

    auto const entries = ParseLog("");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().timestampUnixSeconds == 1'700'000'000);
}

TEST_CASE("FormatReport emits no ANSI escapes when plain")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));

    auto const report = FormatReport("", FastCache::UsageColor::Plain);
    CHECK_FALSE(report.contains("\x1b["));
}

TEST_CASE("FormatReport colors the hit count when colored")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "main", "a.cpp", 10));

    auto const report = FormatReport("", FastCache::UsageColor::Colored);
    CHECK(report.contains("\x1b["));
    // The colored count still contains the plain digits, so a caller stripping
    // ANSI escapes recovers byte-identical text to the plain report.
    CHECK(report.contains("1"));
}

TEST_CASE("FormatReport colors the unavailable count as a warning")
{
    ScopedStateDir const scoped;
    auto record = MakeRecord(Outcome::Unavailable, "main", "a.cpp", 5);
    record.detail = "connect failed";
    AppendRecord(record);

    auto const report = FormatReport("", FastCache::UsageColor::Colored);
    CHECK(report.contains("CACHE NOT REACHED"));
    CHECK(report.contains("\x1b["));
}

TEST_CASE("ParseLog defaults the timestamp to zero for pre-upgrade lines")
{
    ScopedStateDir const scoped;
    // Nine tab-separated fields: the shape written before the timestamp column
    // existed. The parser must not misread a missing trailing field as 0 being
    // a real recorded time — it is simply absent.
    auto const path = LogPath();
    {
        std::ofstream out { path, std::ios::binary | std::ios::app };
        out << "HIT\tmain\t0\t10\ta.cpp\t\t0\t0\t0\t0\n";
    }

    auto const entries = ParseLog("");
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().timestampUnixSeconds == 0);
}
