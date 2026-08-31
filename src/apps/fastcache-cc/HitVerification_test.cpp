// SPDX-License-Identifier: Apache-2.0
#include "HitVerification.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;

namespace
{

/// Write @p bytes to a scratch file.
/// @param dir Where it goes.
/// @param name Its name.
/// @param bytes What it holds.
/// @return The path.
[[nodiscard]] std::filesystem::path WriteFile(std::filesystem::path const& dir,
                                              std::string_view name,
                                              std::string_view bytes)
{
    auto const path = dir / name;
    std::ofstream out { path, std::ios::binary };
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path;
}

/// How many of @p count keys sample at @p rate.
/// @param count How many distinct keys to try.
/// @param rate The sampling rate.
/// @return The number sampled.
[[nodiscard]] std::size_t SampledOut(std::size_t count, unsigned rate)
{
    std::size_t sampled = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (ShouldVerifyHit(std::format("objkey-v3:{:016x}", i), rate))
            ++sampled;
    return sampled;
}

} // namespace

TEST_CASE("Verification is off unless asked for", "[launcher][verify]")
{
    // The default, and it has to be: a verified hit costs a whole compile, so this is
    // for CI, a nightly, or somebody reproducing a report -- never for every build.
    CHECK(ParseVerificationRate("") == VerificationOff);
    CHECK_FALSE(ShouldVerifyHit("any-key", VerificationOff));
    CHECK(SampledOut(1'000, VerificationOff) == 0);
}

TEST_CASE("A rate of one verifies every hit", "[launcher][verify]")
{
    // What somebody reproducing #368 sets. Asserted separately from the modulo below
    // because it is the case a reader checks first and the one a rounding mistake
    // would break.
    CHECK(ParseVerificationRate("1") == 1);
    CHECK(SampledOut(200, 1) == 200);
}

TEST_CASE("The sampled share is about one in the rate", "[launcher][verify]")
{
    // Not an exact count, and it must not be: the whole point of hashing the key is
    // that no counter is shared between the thousands of launcher processes one build
    // spawns, so the rate is statistical rather than exact. What is asserted is the
    // ORDER -- a rule that sampled everything, or nothing, or a tenth of what it was
    // asked for is the failure worth catching, and any of those breaks these bounds.
    constexpr std::size_t Keys = 4'000;
    auto const tenth = SampledOut(Keys, 10);
    CHECK(tenth > Keys / 20);
    CHECK(tenth < Keys / 5);

    // And a rarer rate samples strictly less. Two rates that produced the same share
    // would mean the rate is being ignored, which every bound above would tolerate.
    CHECK(SampledOut(Keys, 100) < tenth);
}

TEST_CASE("Sampling is deterministic, so a reproduction reproduces", "[launcher][verify]")
{
    // The reason this is a hash of the key rather than a draw. `fastcache-cc` is
    // spawned once per translation unit, so a per-process RNG is seeded thousands of
    // times in one build: the rate would be uncontrollable, and -- worse -- a unit
    // that verified once would not verify again, which is exactly what somebody
    // chasing a wrong object needs it to do.
    for (auto const* key: { "objkey-v3:aaaa", "objkey-v3:bbbb", "objkey-v3:cccc" })
    {
        INFO("key " << key);
        CHECK(ShouldVerifyHit(key, 7) == ShouldVerifyHit(key, 7));
    }

    // Different keys are not all the same answer, which is what a hash that ignored
    // its input would produce and what every bound above would still tolerate.
    std::size_t distinct = 0;
    for (auto const* key: { "a", "b", "c", "d", "e", "f", "g", "h" })
        if (ShouldVerifyHit(key, 2))
            ++distinct;
    CHECK(distinct > 0);
    CHECK(distinct < 8);
}

TEST_CASE("A rate that is not a whole number reads as off", "[launcher][verify]")
{
    // The opposite of how this project treats a malformed FLAG, and deliberate: this
    // variable is set by hand while chasing a bug, and a launcher that refused to
    // compile over a typo in it would break the build it was brought in to look at.
    //
    // The WHOLE value has to parse. `100x` reading as 100 would be the launcher doing
    // something other than what was written, silently.
    CHECK(ParseVerificationRate("100x") == VerificationOff);
    CHECK(ParseVerificationRate("x") == VerificationOff);
    CHECK(ParseVerificationRate("-1") == VerificationOff);
    CHECK(ParseVerificationRate("1.5") == VerificationOff);
    CHECK(ParseVerificationRate(" 10") == VerificationOff);
    CHECK(ParseVerificationRate("0") == VerificationOff);

    CHECK(ParseVerificationRate("100") == 100);
}

TEST_CASE("Identical objects match and differing ones do not", "[launcher][verify]")
{
    Testing::ScratchDirectory const scratch { "hit-verify-compare" };
    auto const& dir = scratch.Path();

    auto const served = WriteFile(dir, "served.o", std::string(4096, '\x7f'));
    auto const same = WriteFile(dir, "same.o", std::string(4096, '\x7f'));
    CHECK(CompareObjectFiles(served, same) == HitVerdict::Matched);

    // A single byte, deep inside, and past the first comparison chunk: a comparison
    // that only checked a prefix, or a length, would pass this and is exactly what a
    // wrong object looks like -- an object file that differs in one instruction still
    // links.
    auto differing = std::string(4096, '\x7f');
    differing[3000] = '\x00';
    CHECK(CompareObjectFiles(served, WriteFile(dir, "differs.o", differing)) == HitVerdict::Mismatched);

    // A length difference is a mismatch, not an error.
    CHECK(CompareObjectFiles(served, WriteFile(dir, "short.o", std::string(4095, '\x7f'))) == HitVerdict::Mismatched);
}

TEST_CASE("A comparison that could not be made says so rather than guessing", "[launcher][verify]")
{
    // Four states, not two. Read as `Matched` this would report a verification that
    // did not happen; read as `Mismatched` it would fail a build over a full disk.
    // What it means is "ask again", and only a value of its own can say that.
    Testing::ScratchDirectory const scratch { "hit-verify-missing" };
    auto const& dir = scratch.Path();
    auto const present = WriteFile(dir, "present.o", "abc");

    CHECK(CompareObjectFiles(present, dir / "not-here.o") == HitVerdict::Inconclusive);
    CHECK(CompareObjectFiles(dir / "not-here.o", present) == HitVerdict::Inconclusive);
}

TEST_CASE("A mismatch is described, and says what was linked", "[launcher][verify]")
{
    // The message IS the product here. A wrong object that is counted and not
    // described is a number somebody has to come back and ask about -- and the second
    // half, that the fresh object was used, is what tells a reader whether the build
    // they are holding is trustworthy.
    auto const line = DescribeVerdict(HitVerdict::Mismatched, "objkey-v3:abcdef");
    CHECK(line.contains("objkey-v3:abcdef"));
    CHECK(line.contains("WRONG OBJECT"));
    CHECK(line.contains("freshly compiled"));

    // Nothing to say about the two ordinary outcomes -- a line per hit would make the
    // one that matters unreadable.
    CHECK(DescribeVerdict(HitVerdict::NotChecked, "k").empty());
    CHECK(DescribeVerdict(HitVerdict::Matched, "k").empty());

    // And the inconclusive one is described too, because it is not a pass.
    CHECK_FALSE(DescribeVerdict(HitVerdict::Inconclusive, "objkey-v3:abcdef").empty());
}
