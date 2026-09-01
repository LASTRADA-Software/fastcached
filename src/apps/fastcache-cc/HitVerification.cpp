// SPDX-License-Identifier: Apache-2.0
#include "HitVerification.hpp"

#include "ObjectEquivalence.hpp"

#include <array>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

    /// Read a whole file.
    ///
    /// Reached only once the chunked pass has found a difference, so the "do not
    /// hold two object files in memory" property above still holds for every hit
    /// that verifies clean -- which is all of them, when the cache is right.
    /// @param path What to read.
    /// @return Its bytes, or nothing when it could not be read.
    [[nodiscard]] std::optional<std::vector<std::byte>> ReadWholeFile(std::filesystem::path const& path)
    {
        std::ifstream in { path, std::ios::binary };
        if (!in)
            return std::nullopt;
        std::vector<char> raw { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
        if (in.bad())
            return std::nullopt;
        std::vector<std::byte> bytes;
        bytes.reserve(raw.size());
        for (auto const c: raw)
            bytes.push_back(static_cast<std::byte>(c));
        return bytes;
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
            case ObjectComparison::EquivalentApartFromVolatile: return HitVerdict::Matched;
            case ObjectComparison::Different: return HitVerdict::Mismatched;
            case ObjectComparison::Unsupported: return HitVerdict::Unsupported;
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
    std::ifstream a { served, std::ios::binary };
    std::ifstream b { fresh, std::ios::binary };
    // Inconclusive rather than Mismatched. A file that cannot be opened says nothing
    // about whether the cache is right, and failing a build over a full disk would
    // make this feature the thing operators turn off.
    if (!a || !b)
        return { .verdict = HitVerdict::Inconclusive, .detail = {} };

    // The chunked pass answers the question every hit should get -- identical -- and
    // answers it without holding either file in memory. Only a DIFFERENCE is worth
    // the two buffers, and on ELF a difference is already the whole answer.
    auto differs = false;
    {
        std::array<char, CompareChunk> left {};
        std::array<char, CompareChunk> right {};
        for (;;)
        {
            a.read(left.data(), static_cast<std::streamsize>(left.size()));
            b.read(right.data(), static_cast<std::streamsize>(right.size()));

            auto const readLeft = a.gcount();
            if (readLeft != b.gcount())
            {
                differs = true;
                break;
            }
            if (readLeft == 0)
                break;
            if (std::memcmp(left.data(), right.data(), static_cast<std::size_t>(readLeft)) != 0)
            {
                differs = true;
                break;
            }
        }
    }

    // `bad()` and not `fail()`: a stream that hit end-of-file sets `failbit` on the
    // last short read, which is the ordinary way this loop ends. Only `badbit` says
    // the read itself went wrong, which is the one condition that makes the
    // comparison unanswerable rather than complete.
    if (a.bad() || b.bad())
        return { .verdict = HitVerdict::Inconclusive, .detail = {} };
    if (!differs)
        return { .verdict = HitVerdict::Matched, .detail = {} };

    auto const servedBytes = ReadWholeFile(served);
    auto const freshBytes = ReadWholeFile(fresh);
    if (!servedBytes.has_value() || !freshBytes.has_value())
        return { .verdict = HitVerdict::Inconclusive, .detail = {} };

    auto comparison = CompareObjectImages(*servedBytes, *freshBytes);
    return { .verdict = VerdictOf(comparison.outcome), .detail = std::move(comparison.detail) };
}

std::string DescribeVerdict(HitVerdict verdict, std::string_view key, std::string_view detail)
{
    // One place decides whether a detail is appended, so a new verdict cannot ship a
    // sentence that trails a bare full stop or swallows what the comparison found.
    auto const withDetail = [detail](std::string sentence) {
        return detail.empty() ? sentence : std::format("{} -- {}", sentence, detail);
    };

    switch (verdict)
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
            return withDetail(
                std::format("fastcache-cc: could not verify the hit for key {} -- the fresh compile or the "
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
