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

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

using namespace FastCache::Cc;

namespace
{

/// This process's id, so two test *processes* racing under a parallel test
/// runner (`ctest -j`) can never compute the same directory name.
///
/// `catch_discover_tests` registers one CTest test per TEST_CASE, each its own
/// process invocation of this binary — so a monotonic counter starting at 1 in
/// every process is not actually monotonic across the run: two concurrently
/// running single-test processes both take their first ScopedStateDir at
/// counter value 1 and collide on the same directory, one deleting or
/// overwriting the log the other is mid-write on. This was a real, reproduced
/// flake (`ctest -R FormatReport -j8` fails a run in a small majority of
/// tries), not a hypothetical.
[[nodiscard]] unsigned long ProcessId() noexcept
{
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

/// Monotonic counter so no two ScopedStateDir instances *within this process*
/// share a directory; combined with the process id for cross-process safety.
[[nodiscard]] int CounterNext()
{
    static int counter = 0;
    return ++counter;
}

/// Redirects the statistics log into a temporary directory for one test, and
/// restores the previous environment afterwards.
class ScopedStateDir
{
  public:
    ScopedStateDir()
    {
        auto const unique =
            std::filesystem::temp_directory_path() / std::format("fastcache-cc-test-{}-{}", ProcessId(), CounterNext());
        std::filesystem::create_directories(unique);
        _dir = unique.string();

        _previous = Current();
        Set(_dir);
        static_cast<void>(ResetLog()); // start from a known-empty log
    }

    ~ScopedStateDir()
    {
        if (_previous.has_value())
            Set(*_previous);
        else
            Unset();
        std::error_code ec;
        std::filesystem::remove_all(_dir, ec);
    }

    ScopedStateDir(ScopedStateDir const&) = delete;
    ScopedStateDir& operator=(ScopedStateDir const&) = delete;
    ScopedStateDir(ScopedStateDir&&) = delete;
    ScopedStateDir& operator=(ScopedStateDir&&) = delete;

  private:
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

    std::string _dir;
    std::optional<std::string> _previous;
};

/// Build a record with the fields these tests care about.
[[nodiscard]] Record MakeRecord(Outcome outcome, std::string cohort, std::string source, std::uint64_t elapsedMs)
{
    Record record;
    record.outcome = outcome;
    record.cohort = std::move(cohort);
    record.source = std::move(source);
    record.elapsedMs = elapsedMs;
    return record;
}

} // namespace

TEST_CASE("ToStringView round-trips every outcome token")
{
    CHECK(ToStringView(Outcome::Hit) == "HIT");
    CHECK(ToStringView(Outcome::Miss) == "MISS");
    CHECK(ToStringView(Outcome::Uncacheable) == "UNCACHEABLE");
    CHECK(ToStringView(Outcome::Unavailable) == "UNAVAILABLE");
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

TEST_CASE("FormatReport restricts the fold to one cohort when filtered")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "alpha", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "b.cpp", 20));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "c.cpp", 30));

    auto const alpha = FormatReport("alpha");
    CHECK(alpha.contains("compiles     : 1"));

    auto const beta = FormatReport("beta");
    CHECK(beta.contains("compiles     : 2"));

    // A cohort with no records says so rather than reporting an empty fold.
    CHECK(FormatReport("gamma").contains("no records for cohort 'gamma'"));
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

TEST_CASE("FormatHtmlReport lists every cohort in the comparison table")
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

TEST_CASE("FormatHtmlReport restricts the fold to one cohort when filtered")
{
    ScopedStateDir const scoped;
    AppendRecord(MakeRecord(Outcome::Hit, "alpha", "a.cpp", 10));
    AppendRecord(MakeRecord(Outcome::Miss, "beta", "b.cpp", 20));

    auto const alpha = FormatHtmlReport("alpha");
    CHECK(alpha.contains("alpha"));
    CHECK_FALSE(alpha.contains(">beta<"));

    CHECK(FormatHtmlReport("gamma").contains("no records for cohort 'gamma'"));
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
