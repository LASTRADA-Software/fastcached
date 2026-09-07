// SPDX-License-Identifier: Apache-2.0
#include "RefusalNotice.hpp"

#include <algorithm>
#include <fstream>
#include <string>

namespace FastCache::Cc
{

namespace
{
    /// A filename-safe stamp name for one (endpoint, code) pair.
    ///
    /// The endpoint is folded to a hash rather than spelled: it can carry `:` and
    /// `/`, which are not filename characters everywhere this runs, and a host name
    /// is unbounded while a filename is not.
    [[nodiscard]] std::string StampName(std::string_view endpoint, CompileCacheWire::ErrorCode code)
    {
        // FNV-1a, inline: this needs to be stable within one build's processes and
        // nothing else, so it owes no cryptographic property and no cross-version
        // agreement -- a changed hash costs one extra line, once.
        std::uint64_t h = 1469598103934665603ULL;
        for (auto const c: endpoint)
        {
            h ^= static_cast<std::uint8_t>(c);
            h *= 1099511628211ULL;
        }
        return std::format("refusal-{:016x}-{:02x}.stamp", h, static_cast<std::uint8_t>(code));
    }
} // namespace

PersistentRefusal const* PersistentRefusalFor(CompileCacheWire::ErrorCode code) noexcept
{
    for (auto const& row: PersistentRefusalTable)
        if (row.code == code)
            return &row;
    return nullptr;
}

bool ShouldAnnounceRefusal(std::filesystem::path const& stateDir,
                           std::string_view endpoint,
                           CompileCacheWire::ErrorCode code,
                           std::chrono::system_clock::time_point now,
                           std::chrono::seconds interval)
{
    // No state directory means no throttle, and the answer is YES rather than no.
    // This function exists to break a silence; a machine that cannot persist the
    // stamp is exactly the one where suppressing the line makes it permanent.
    if (stateDir.empty())
        return true;

    auto const stamp = stateDir / StampName(endpoint, code);
    auto const nowSeconds = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

    // Read first. Every failure to read -- absent, unreadable, garbage -- means
    // ANNOUNCE, for the same reason the empty directory does: this is a noise
    // ceiling, not a correctness mechanism, and failing it closed would restore the
    // silence.
    std::error_code ec;
    if (std::ifstream in { stamp }; in)
    {
        long long last = 0;
        if (in >> last && nowSeconds - last < interval.count())
            return false;
    }

    // Best effort, and its failure is not reported: statistics and notices must
    // never break a build. A write that fails simply means the next unit asks again.
    std::filesystem::create_directories(stateDir, ec);
    if (std::ofstream out { stamp, std::ios::trunc }; out)
        out << nowSeconds << '\n';
    return true;
}

std::string RefusalNoticeLine(std::string_view endpoint, PersistentRefusal const& row, std::string_view detail)
{
    // The daemon's own words are included when it sent any, because a refusal that
    // names a supported version range is far more useful than the category alone --
    // and they are bounded by cause rather than by request, which is the same
    // argument `DescribeOutcome` makes for putting them in the statistics reason.
    auto const said = detail.empty() ? std::string {} : std::format(" ({})", detail);
    return std::format("fastcache-cc: the cache at {} is refusing every request{}; {}. "
                       "This build is compiling without a cache.",
                       endpoint,
                       said,
                       row.remedy);
}

} // namespace FastCache::Cc
