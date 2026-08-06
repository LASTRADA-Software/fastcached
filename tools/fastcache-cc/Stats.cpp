// SPDX-License-Identifier: Apache-2.0
#include "Stats.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache::Cc
{
namespace
{

    constexpr char FieldSeparator = '\t';

    /// Replace characters that would corrupt the one-record-per-line format.
    [[nodiscard]] std::string Sanitize(std::string_view text)
    {
        std::string out { text };
        std::ranges::replace(out, '\t', ' ');
        std::ranges::replace(out, '\r', ' ');
        std::ranges::replace(out, '\n', ' ');
        return out;
    }

    /// Directory holding the log, created on demand. Empty on failure.
    [[nodiscard]] std::filesystem::path StateDirectory()
    {
        std::error_code ec;
#if defined(_WIN32)
        std::size_t size = 0;
        if (::getenv_s(&size, nullptr, 0, "LOCALAPPDATA") != 0 || size == 0)
            return {};
        std::string base(size, '\0');
        if (::getenv_s(&size, base.data(), base.size(), "LOCALAPPDATA") != 0)
            return {};
        base.resize(size > 0 ? size - 1 : 0);
#else
        char const* xdg = std::getenv("XDG_STATE_HOME");
        char const* home = std::getenv("HOME");
        std::string base = (xdg != nullptr && xdg[0] != '\0') ? std::string { xdg }
                           : (home != nullptr)                ? std::string { home } + "/.local/state"
                                                              : std::string {};
#endif
        if (base.empty())
            return {};
        std::filesystem::path dir = std::filesystem::path { base } / "fastcache-cc";
        std::filesystem::create_directories(dir, ec);
        return ec ? std::filesystem::path {} : dir;
    }

    /// Per-cohort tallies folded from the log.
    struct Tally
    {
        std::uint64_t hits {};
        std::uint64_t misses {};
        std::uint64_t uncacheable {};
        std::uint64_t unavailable {};

        // Full sample sets, not running sums: the point of the distribution is the
        // shape (a bimodal miss profile means something an average hides).
        std::vector<std::uint64_t> hitMs;
        std::vector<std::uint64_t> missMs;

        // Phase split of the hit path. A hit still preprocesses to derive its key,
        // so total hit latency alone cannot say whether the cache or the compiler
        // front end is the slow part.
        std::vector<std::uint64_t> hitPreprocessMs;
        std::vector<std::uint64_t> hitCacheMs;

        /// Hits served through the manifest shortcut, and how long validating it
        /// took. Split out because these hits skip preprocessing entirely, so
        /// averaging them with preprocessed hits would hide the improvement.
        std::uint64_t directHits {};
        std::vector<std::uint64_t> directMs;

        /// Fall-back reason -> count, so an "unavailable" figure is actionable.
        std::map<std::string, std::uint64_t> reasons;

        [[nodiscard]] std::uint64_t Total() const noexcept
        {
            return hits + misses + uncacheable + unavailable;
        }
    };

    [[nodiscard]] std::uint64_t ParseUnsigned(std::string_view text)
    {
        std::uint64_t value = 0;
        std::from_chars(text.data(), text.data() + text.size(), value);
        return value;
    }

    [[nodiscard]] Outcome ParseOutcome(std::string_view token)
    {
        if (token == "HIT")
            return Outcome::Hit;
        if (token == "MISS")
            return Outcome::Miss;
        if (token == "UNCACHEABLE")
            return Outcome::Uncacheable;
        return Outcome::Unavailable;
    }

    /// Split a log line into its tab-separated fields.
    [[nodiscard]] std::vector<std::string_view> SplitFields(std::string_view line)
    {
        std::vector<std::string_view> fields;
        std::size_t start = 0;
        while (start <= line.size())
        {
            auto const end = line.find(FieldSeparator, start);
            if (end == std::string_view::npos)
            {
                fields.emplace_back(line.substr(start));
                break;
            }
            fields.emplace_back(line.substr(start, end - start));
            start = end + 1;
        }
        return fields;
    }

    /// Render a percentage with one decimal, guarding the empty case.
    [[nodiscard]] std::string Percent(std::uint64_t part, std::uint64_t whole)
    {
        if (whole == 0)
            return "n/a";
        double const pct = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
        std::array<char, 32> buffer {};
        auto const written = std::snprintf(buffer.data(), buffer.size(), "%.1f%%", pct);
        return written > 0 ? std::string { buffer.data(), static_cast<std::size_t>(written) } : "n/a";
    }

    /// Format one millisecond boundary compactly: sub-second in ms, above in s with a
    /// decimal only where the value is not a whole second.
    [[nodiscard]] std::string FormatMs(std::uint64_t ms)
    {
        std::array<char, 32> buffer {};
        int written = 0;
        if (ms < 1000)
            written = std::snprintf(buffer.data(), buffer.size(), "%llums", static_cast<unsigned long long>(ms));
        else if (ms % 1000 == 0)
            written = std::snprintf(buffer.data(), buffer.size(), "%llus", static_cast<unsigned long long>(ms / 1000));
        else
            written = std::snprintf(buffer.data(), buffer.size(), "%.1fs", static_cast<double>(ms) / 1000.0);
        return written > 0 ? std::string { buffer.data(), static_cast<std::size_t>(written) } : "?";
    }

    /// The percentile of a sorted sample set, by nearest-rank.
    [[nodiscard]] std::uint64_t Percentile(std::vector<std::uint64_t> const& sorted, double fraction)
    {
        if (sorted.empty())
            return 0;
        auto const scaled = fraction * static_cast<double>(sorted.size() - 1);
        auto const index = static_cast<std::size_t>(scaled + 0.5);
        return sorted[index < sorted.size() ? index : sorted.size() - 1];
    }

    /// Ramp of ASCII characters, ordered by visual weight, so one character conveys a
    /// bucket's height.
    ///
    /// Deliberately ASCII rather than Unicode block elements: the launcher writes to
    /// whatever console the build runs in, and a Windows console using a legacy ANSI
    /// code page renders UTF-8 block bytes as mojibake (`▄` arrives as `Γûä`), which
    /// made the report unreadable in exactly the place it is most used.
    constexpr std::array<char, 8> RampGlyphs { '.', ':', '-', '=', '+', '*', '#', '@' };

    /// Render one latency sample set as a single-line sparkline over buckets derived
    /// from the data itself.
    ///
    /// The bucket edges are linear across the observed min..max rather than a fixed
    /// ladder: a fixed ladder's widest bucket swallowed whatever it could not
    /// resolve, so a 2 s hit and a 240 s hit both read as ">10s" and the shape of a
    /// tight cluster collapsed into one bar. Deriving the edges keeps the resolution
    /// on the range that actually occurred.
    void AppendHistogram(std::ostringstream& out,
                         std::string_view title,
                         std::vector<std::uint64_t> const& samples,
                         std::string_view indent)
    {
        if (samples.empty())
            return;

        std::vector<std::uint64_t> sorted { samples };
        std::ranges::sort(sorted);
        auto const low = sorted.front();
        auto const high = sorted.back();

        // A degenerate range (every sample identical) has no shape to plot; the
        // summary line still carries the value, so report it without a sparkline.
        if (low == high)
        {
            out << indent << title << "  " << samples.size() << " samples  all " << FormatMs(low) << '\n';
            return;
        }

        constexpr std::size_t BucketCount = 24;
        std::array<std::uint64_t, BucketCount> counts {};
        auto const span = high - low;
        for (std::uint64_t const sample: samples)
        {
            // Scale into [0, BucketCount-1]; the max sample lands in the last bucket.
            auto const offset = static_cast<double>(sample - low) / static_cast<double>(span);
            auto index = static_cast<std::size_t>(offset * static_cast<double>(BucketCount));
            if (index >= BucketCount)
                index = BucketCount - 1;
            ++counts[index];
        }

        auto const peak = *std::ranges::max_element(counts);
        if (peak == 0)
            return;

        std::string spark;
        for (auto const count: counts)
        {
            if (count == 0)
            {
                spark += ' ';
                continue;
            }
            // Non-empty buckets start at glyph 1, so a single outlier stays visible
            // next to a tall neighbour instead of rendering as the faintest mark.
            auto const scaled = (static_cast<double>(count) / static_cast<double>(peak)) * (RampGlyphs.size() - 1);
            auto const glyph = static_cast<std::size_t>(scaled + 0.5);
            spark += RampGlyphs[glyph < RampGlyphs.size() ? glyph : RampGlyphs.size() - 1];
        }

        out << indent << title << "  " << samples.size() << " samples  " << FormatMs(low) << '-' << FormatMs(high) << '\n';
        out << indent << "  " << spark << "  p50=" << FormatMs(Percentile(sorted, 0.50))
            << " p95=" << FormatMs(Percentile(sorted, 0.95)) << " max=" << FormatMs(high) << '\n';
    }

    void AppendTallyLines(std::ostringstream& out, Tally const& tally)
    {
        // Rate the cache against the compiles it could actually serve. Dividing by
        // every invocation blends "the cache did not have it" with "the cache was
        // unreachable", so an outage reads as a poor hit rate and hides its own cause.
        auto const servable = tally.hits + tally.misses;

        out << "  compiles     : " << tally.Total() << '\n'
            << "  hits         : " << tally.hits << "  (" << Percent(tally.hits, servable) << " of " << servable
            << " cacheable)\n";
        if (tally.directHits > 0)
            out << "    via direct : " << tally.directHits << "  (" << Percent(tally.directHits, tally.hits)
                << " of hits, no preprocess)\n";
        out << "  misses       : " << tally.misses << '\n';
        if (tally.uncacheable > 0)
            out << "  uncacheable  : " << tally.uncacheable << '\n';
        if (tally.unavailable > 0)
            out << "  unavailable  : " << tally.unavailable << "  (" << Percent(tally.unavailable, tally.Total())
                << " of all compiles -- CACHE NOT REACHED)\n";

        if (!tally.reasons.empty())
        {
            out << "  fall-back reasons\n";
            std::vector<std::pair<std::string, std::uint64_t>> ranked { tally.reasons.begin(), tally.reasons.end() };
            std::ranges::sort(ranked, [](auto const& a, auto const& b) { return a.second > b.second; });
            for (auto const& [reason, count]: ranked)
                out << "    " << count << "x  " << reason << '\n';
        }

        if (!tally.hitMs.empty() || !tally.missMs.empty())
        {
            out << '\n';
            AppendHistogram(out, "hit latency  ", tally.hitMs, "  ");
            // Attribute a slow hit: preprocessing to derive the key is unavoidable
            // work the cache cannot remove, so showing it beside the cache time says
            // whether tuning the daemon can help at all.
            AppendHistogram(out, "  preprocess ", tally.hitPreprocessMs, "  ");
            AppendHistogram(out, "  cache i/o  ", tally.hitCacheMs, "  ");
            AppendHistogram(out, "  direct val ", tally.directMs, "  ");
            if (!tally.hitMs.empty() && !tally.missMs.empty())
                out << '\n';
            AppendHistogram(out, "miss latency ", tally.missMs, "  ");
        }
    }

} // namespace

std::string_view ToStringView(Outcome outcome) noexcept
{
    switch (outcome)
    {
        case Outcome::Hit:
            return "HIT";
        case Outcome::Miss:
            return "MISS";
        case Outcome::Uncacheable:
            return "UNCACHEABLE";
        case Outcome::Unavailable:
            break;
    }
    return "UNAVAILABLE";
}

std::string LogPath()
{
    auto const dir = StateDirectory();
    if (dir.empty())
        return {};
    return (dir / "invocations.log").string();
}

void AppendRecord(Record const& record)
{
    auto const path = LogPath();
    if (path.empty())
        return;

    std::string line;
    line += ToStringView(record.outcome);
    line += FieldSeparator;
    line += Sanitize(record.cohort);
    line += FieldSeparator;
    line += std::to_string(record.valueBytes);
    line += FieldSeparator;
    line += std::to_string(record.elapsedMs);
    line += FieldSeparator;
    line += Sanitize(record.source);
    line += FieldSeparator;
    line += Sanitize(record.detail);
    line += FieldSeparator;
    line += std::to_string(record.preprocessMs);
    line += FieldSeparator;
    line += std::to_string(record.cacheMs);
    line += FieldSeparator;
    line += std::to_string(record.directMs);
    line += FieldSeparator;
    line += record.directHit ? "1" : "0";
    line += '\n';

#if defined(_WIN32)
    // FILE_APPEND_DATA without FILE_WRITE_DATA makes each write atomically land
    // at end-of-file, so the ~500 concurrent compilers of one build interleave
    // whole lines instead of shredding each other's. A plain seek-then-write
    // (std::ofstream::app) has no such guarantee across processes.
    HANDLE handle = ::CreateFileA(path.c_str(),
                                  FILE_APPEND_DATA,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr,
                                  OPEN_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    ::WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    ::CloseHandle(handle);
#else
    // O_APPEND gives the same atomicity for writes under PIPE_BUF on POSIX.
    std::FILE* file = std::fopen(path.c_str(), "ae");
    if (file == nullptr)
        return;
    std::fwrite(line.data(), 1, line.size(), file);
    std::fclose(file);
#endif
}

std::string FormatReport(std::string_view cohortFilter)
{
    auto const path = LogPath();
    if (path.empty())
        return "fastcache-cc: no state directory available; statistics are disabled.\n";

    std::ifstream input { path, std::ios::binary };
    if (!input)
        return "fastcache-cc: no statistics recorded yet (" + path + ").\n";

    Tally overall;
    std::map<std::string, Tally> byCohort;
    std::map<std::string, std::uint64_t> neverCached;

    std::string line;
    while (std::getline(input, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;

        auto const fields = SplitFields(line);
        if (fields.size() < 4)
            continue;

        std::string const cohort { fields[1] };
        if (!cohortFilter.empty() && cohort != cohortFilter)
            continue;

        auto const outcome = ParseOutcome(fields[0]);
        auto const elapsed = ParseUnsigned(fields[3]);
        std::string const reason { fields.size() >= 6 ? fields[5] : std::string_view {} };

        for (Tally* tally: { &overall, &byCohort[cohort] })
        {
            switch (outcome)
            {
                case Outcome::Hit:
                    ++tally->hits;
                    tally->hitMs.push_back(elapsed);
                    // Older log lines predate the phase columns; absent fields simply
                    // leave the phase histograms empty rather than skewing them to 0.
                    if (fields.size() >= 8)
                    {
                        tally->hitPreprocessMs.push_back(ParseUnsigned(fields[6]));
                        tally->hitCacheMs.push_back(ParseUnsigned(fields[7]));
                    }
                    if (fields.size() >= 10 && fields[9] == "1")
                    {
                        ++tally->directHits;
                        tally->directMs.push_back(ParseUnsigned(fields[8]));
                    }
                    break;
                case Outcome::Miss:
                    ++tally->misses;
                    tally->missMs.push_back(elapsed);
                    break;
                case Outcome::Uncacheable:
                    ++tally->uncacheable;
                    break;
                case Outcome::Unavailable:
                    ++tally->unavailable;
                    break;
            }
            if (!reason.empty())
                ++tally->reasons[reason];
        }

        // Attribute the never-cached translation units so a permanently
        // uncacheable file is visible rather than hidden in a percentage.
        if ((outcome == Outcome::Uncacheable || outcome == Outcome::Unavailable) && fields.size() >= 5 && !fields[4].empty())
            ++neverCached[std::string { fields[4] }];
    }

    if (overall.Total() == 0)
    {
        if (cohortFilter.empty())
            return "fastcache-cc: no statistics recorded yet (" + path + ").\n";
        return "fastcache-cc: no records for cohort '" + std::string { cohortFilter } + "'.\n";
    }

    std::ostringstream out;
    out << "fastcache-cc statistics (" << path << ")\n\n";
    if (cohortFilter.empty())
        out << "all cohorts\n";
    else
        out << "cohort " << cohortFilter << '\n';
    AppendTallyLines(out, overall);

    if (cohortFilter.empty() && byCohort.size() > 1)
    {
        out << "\nper cohort\n";
        for (auto const& [cohort, tally]: byCohort)
        {
            out << "\n  " << (cohort.empty() ? "(unset)" : cohort) << '\n';
            std::ostringstream nested;
            AppendTallyLines(nested, tally);
            std::istringstream lines { nested.str() };
            std::string nestedLine;
            while (std::getline(lines, nestedLine))
                out << "  " << nestedLine << '\n';
        }
    }

    if (!neverCached.empty())
    {
        out << "\nnever cached (" << neverCached.size() << " translation units)\n";
        std::vector<std::pair<std::string, std::uint64_t>> ranked { neverCached.begin(), neverCached.end() };
        std::ranges::sort(ranked, [](auto const& a, auto const& b) { return a.second > b.second; });
        std::size_t shown = 0;
        for (auto const& [source, count]: ranked)
        {
            if (shown++ >= 10)
            {
                out << "  ... and " << (ranked.size() - 10) << " more\n";
                break;
            }
            out << "  " << count << "x  " << source << '\n';
        }
    }

    return out.str();
}

bool ResetLog()
{
    auto const path = LogPath();
    if (path.empty())
        return false;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !std::filesystem::exists(path);
}

} // namespace FastCache::Cc
