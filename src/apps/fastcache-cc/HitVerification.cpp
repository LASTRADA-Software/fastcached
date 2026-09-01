// SPDX-License-Identifier: Apache-2.0
#include "FileBytes.hpp"
#include "HitVerification.hpp"
#include "ObjectEquivalence.hpp"

#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

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

    /// What an image comparison means for a hit.
    ///
    /// A table rather than a cast, because the two enumerations answer different
    /// questions and only happen to have neighbouring sizes: `Identical` and
    /// `EquivalentApartFromVolatile` are one verdict, and folding them the other way
    /// -- reporting the ordinary Windows hit as a wrong object -- is #493 itself.
    /// No `default:`, so a new outcome is a compile error here rather than a silent
    /// arm.
    /// @param outcome What comparing the two images found.
    /// @return The verdict to report.
    [[nodiscard]] HitVerdict VerdictOf(ObjectComparison outcome) noexcept
    {
        switch (outcome)
        {
            case ObjectComparison::Identical:
            case ObjectComparison::EquivalentApartFromVolatile:
                return HitVerdict::Matched;
            case ObjectComparison::Different:
                return HitVerdict::Mismatched;
            case ObjectComparison::Unsupported:
                return HitVerdict::Unsupported;
        }
        return HitVerdict::Inconclusive;
    }
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

HitComparison CompareObjectFiles(std::filesystem::path const& served, std::filesystem::path const& fresh)
{
    auto const servedBytes = ReadFileBytes(served);
    auto const freshBytes = ReadFileBytes(fresh);
    // Inconclusive rather than Mismatched. A file that cannot be read says nothing
    // about whether the cache is right, and failing a build over a full disk would
    // make this feature the thing operators turn off.
    if (!servedBytes.has_value() || !freshBytes.has_value())
        return { .verdict = HitVerdict::Inconclusive, .comparison = std::nullopt, .detail = {} };

    auto comparison = CompareObjectImages(*servedBytes, *freshBytes);
    return { .verdict = VerdictOf(comparison.outcome),
             .comparison = comparison.outcome,
             .detail = std::move(comparison.detail) };
}

std::string DescribeVerdict(HitComparison const& comparison, std::string_view key)
{
    // One place decides whether a detail is appended, so a new verdict cannot ship a
    // sentence that trails a bare full stop or swallows what the comparison found.
    auto const withDetail = [&comparison](std::string sentence) {
        return comparison.detail.empty() ? std::move(sentence) : std::format("{} -- {}", sentence, comparison.detail);
    };

    switch (comparison.verdict)
    {
        case HitVerdict::NotChecked:
        case HitVerdict::Matched:
            return {};
        case HitVerdict::Mismatched:
            // Says what was done about it, not only what was found. A line reporting a
            // wrong object without saying which one was linked leaves a reader unable
            // to decide whether the build they are holding is trustworthy.
            return withDetail(
                std::format("fastcache-cc: WRONG OBJECT served for key {} -- the cache's object differs from what "
                            "this compiler produces from this source. Using the freshly compiled one; the cached "
                            "entry is wrong and this build is unaffected",
                            key));
        case HitVerdict::Inconclusive:
            return withDetail(std::format("fastcache-cc: could not verify the hit for key {} -- the fresh compile or the "
                                          "comparison did not complete. Nothing is known about the cached object either way",
                                          key));
        case HitVerdict::Unsupported:
            // NOT phrased as a finding about the cache, and that is the whole point of
            // the state: an operator who reads this as a mismatch turns verification
            // off, and the class it guards goes invisible again.
            return withDetail(std::format("fastcache-cc: cannot verify hits for this toolchain, so the hit for key "
                                          "{} was neither confirmed nor faulted",
                                          key));
    }
    return {};
}

} // namespace FastCache::Cc
