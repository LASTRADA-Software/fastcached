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
#include <optional>
#include <string>
#include <system_error>

using namespace FastCache::Cc;

namespace
{

/// Monotonic counter so no two tests share a state directory.
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
            std::filesystem::temp_directory_path() / ("fastcache-cc-test-" + std::to_string(CounterNext()));
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

TEST_CASE("LogPath points inside the configured state directory")
{
    ScopedStateDir const scoped;
    auto const path = LogPath();
    CHECK_FALSE(path.empty());
    CHECK(path.contains("fastcache-cc"));
}
