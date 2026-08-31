// SPDX-License-Identifier: Apache-2.0
#include "HitVerification.hpp"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <string>

namespace FastCache::Cc
{

namespace
{
    /// FNV-1a over the key, spelled here rather than reached for.
    ///
    /// `std::hash` is deliberately not used: the standard leaves it unspecified, so
    /// two machines in one fleet could sample different translation units and a
    /// reproduction on one would not reproduce on the other -- which is the single
    /// thing somebody chasing a wrong object needs. `Core/MurmurHash3` would do, and
    /// is not reachable: `fastcache-cc` does not link `FastCache`, compiling in only a
    /// few dependency-free leaves, and a sampling rule is not worth widening that set
    /// for. Four lines of a fully specified hash is the cheaper of the two.
    ///
    /// Not a cryptographic choice and not one that needs to be. Nothing is being
    /// authenticated; the key is not adversarial, and the property wanted is that
    /// similar keys land in different buckets.
    /// @param text What to hash.
    /// @return The digest.
    [[nodiscard]] std::uint64_t Fnv1a(std::string_view text) noexcept
    {
        constexpr std::uint64_t Offset = 14'695'981'039'346'656'037ULL;
        constexpr std::uint64_t Prime = 1'099'511'628'211ULL;

        auto hash = Offset;
        for (auto const byte: text)
        {
            hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
            hash *= Prime;
        }
        return hash;
    }

    /// How many bytes to compare at a time.
    ///
    /// An object file is kilobytes to a few megabytes, so this is about not holding
    /// two whole files in memory rather than about throughput.
    constexpr std::size_t CompareChunk = 64U * 1024U;
} // namespace

bool ShouldVerifyHit(std::string_view key, unsigned rate) noexcept
{
    if (rate == VerificationOff)
        return false;
    // Rate 1 verifies everything, which is what somebody reproducing a report sets --
    // and the modulo would say so anyway. Written out because "one in one" is the case
    // a reader checks first.
    if (rate == 1)
        return true;
    return Fnv1a(key) % rate == 0;
}

unsigned ParseVerificationRate(std::string_view text) noexcept
{
    if (text.empty())
        return VerificationOff;

    // Spelled out rather than handed to `from_chars`, and the reason is the contract
    // rather than the call: EVERY character has to be a digit. `from_chars` reports
    // where it stopped, so the caller has to check that too -- and a caller that
    // forgot would read `100x` as 100, which is the launcher silently doing something
    // other than what was written. Ten lines that cannot be got wrong beat four that
    // can.
    std::uint64_t rate = 0;
    for (auto const c: text)
    {
        if (c < '0' || c > '9')
            return VerificationOff;
        rate = (rate * 10) + static_cast<std::uint64_t>(c - '0');
        // A rate nobody could mean. Refused rather than wrapped, because a wrapped
        // value would be a rate the operator did not ask for and could not predict.
        if (rate > std::numeric_limits<unsigned>::max())
            return VerificationOff;
    }
    return static_cast<unsigned>(rate);
}

HitVerdict CompareObjectFiles(std::filesystem::path const& served, std::filesystem::path const& fresh)
{
    std::ifstream a { served, std::ios::binary };
    std::ifstream b { fresh, std::ios::binary };
    // Inconclusive rather than Mismatched. A file that cannot be opened says nothing
    // about whether the cache is right, and failing a build over a full disk would
    // make this feature the thing operators turn off.
    if (!a || !b)
        return HitVerdict::Inconclusive;

    std::array<char, CompareChunk> left {};
    std::array<char, CompareChunk> right {};
    for (;;)
    {
        a.read(left.data(), static_cast<std::streamsize>(left.size()));
        b.read(right.data(), static_cast<std::streamsize>(right.size()));

        // A short read from one and not the other is a length difference, which is a
        // mismatch and not an error -- both files are ordinary local files and neither
        // is being written while this runs.
        auto const readLeft = a.gcount();
        if (readLeft != b.gcount())
            return HitVerdict::Mismatched;
        if (readLeft == 0)
            break;
        if (std::memcmp(left.data(), right.data(), static_cast<std::size_t>(readLeft)) != 0)
            return HitVerdict::Mismatched;
    }

    // `bad()` and not `fail()`: a stream that hit end-of-file sets `failbit` on the
    // last short read, which is the ordinary way this loop ends. Only `badbit` says
    // the read itself went wrong, which is the one condition that makes the
    // comparison unanswerable rather than complete.
    if (a.bad() || b.bad())
        return HitVerdict::Inconclusive;
    return HitVerdict::Matched;
}

std::string DescribeVerdict(HitVerdict verdict, std::string_view key)
{
    switch (verdict)
    {
        case HitVerdict::NotChecked:
        case HitVerdict::Matched:
            return {};
        case HitVerdict::Mismatched:
            // Says what was done about it, not only what was found. A line reporting a
            // wrong object without saying which one was linked leaves a reader unable
            // to decide whether the build they are holding is trustworthy.
            return std::format("fastcache-cc: WRONG OBJECT served for key {} -- the cache's object differs from what "
                               "this compiler produces from this source. Using the freshly compiled one; the cached "
                               "entry is wrong and this build is unaffected",
                               key);
        case HitVerdict::Inconclusive:
            return std::format("fastcache-cc: could not verify the hit for key {} -- the fresh compile or the "
                               "comparison did not complete. Nothing is known about the cached object either way",
                               key);
    }
    return {};
}

} // namespace FastCache::Cc
