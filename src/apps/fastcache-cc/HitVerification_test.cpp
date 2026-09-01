// SPDX-License-Identifier: Apache-2.0
#include "HitVerification.hpp"
#include "StubCoffTestSupport.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Cc;
using namespace FastCache::Cc::Test;

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

/// The sections the file-layer cases use: one, carrying data.
///
/// The builder itself is `StubCoffTestSupport.hpp` -- shared with
/// `ObjectEquivalence_test.cpp`, where the format is asserted. These cases are about
/// reaching the object comparison through the FILE layer, not about COFF.
/// @return One section of data.
[[nodiscard]] std::vector<StubSection> OneCodeSection()
{
    return { { .name = ".text$mn", .data = "CODE-AND-DATA" } };
}

/// A stub object as a byte string, for `WriteFile`.
/// @param timestamp What to put in `TimeDateStamp`.
/// @return The image.
[[nodiscard]] std::string StubCoff(std::uint32_t timestamp)
{
    auto const image = BuildCoff(OneCodeSection(), timestamp);
    std::string bytes;
    std::ranges::transform(image, std::back_inserter(bytes), [](std::byte byte) { return static_cast<char>(byte); });
    return bytes;
}

/// Where `StubCoff` puts a byte that is section DATA.
///
/// Derived from the builder's own constants rather than spelled out, so a corruption
/// case cannot quietly start flipping a byte in the header -- part of which the
/// comparison is allowed to overlook, which would make the case pass for the
/// opposite of its stated reason.
constexpr std::size_t CodeByteOffset = StubFirstSectionDataAt(1) + 2;

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
    CHECK(CompareObjectFiles(served, same).verdict == HitVerdict::Matched);

    // A single byte, deep inside: a comparison that only checked a prefix, or a
    // length, would pass this and is exactly what a wrong object looks like -- an
    // object file that differs in one instruction still links.
    auto differing = std::string(4096, '\x7f');
    differing[3000] = '\x00';
    CHECK(CompareObjectFiles(served, WriteFile(dir, "differs.o", differing)).verdict == HitVerdict::Mismatched);

    // A length difference is a mismatch, not an error.
    CHECK(CompareObjectFiles(served, WriteFile(dir, "short.o", std::string(4095, '\x7f'))).verdict
          == HitVerdict::Mismatched);
}

TEST_CASE("A hit whose object differs only in the COFF clock verifies clean", "[launcher][verify]")
{
    // #493, through the file layer this time: the chunked pass finds a difference and
    // hands over to the object comparison rather than answering `Mismatched` on it.
    // Without that handover every Windows hit reported a wrong object, because a
    // cached object is older than the fresh compile it is checked against BY
    // CONSTRUCTION -- which made the one instrument that can see a wrong object
    // useless on the platform where one was observed.
    Testing::ScratchDirectory const scratch { "hit-verify-clock" };
    auto const& dir = scratch.Path();

    auto const served = WriteFile(dir, "served.obj", StubCoff(1000));
    auto const fresh = WriteFile(dir, "fresh.obj", StubCoff(2000));

    auto const clean = CompareObjectFiles(served, fresh);
    CHECK(clean.verdict == HitVerdict::Matched);
    CHECK_FALSE(clean.detail.empty());

    // And the guard still bites: the same clock difference plus one byte of section
    // data is a wrong object. A verifier that stopped crying wolf by no longer
    // looking would pass the case above and fail this one.
    auto corrupted = StubCoff(2000);
    corrupted[CodeByteOffset] = 'Z';
    auto const caught = CompareObjectFiles(served, WriteFile(dir, "wrong.obj", corrupted));
    CHECK(caught.verdict == HitVerdict::Mismatched);
    CHECK_FALSE(caught.detail.empty());
}

TEST_CASE("A comparison that could not be made says so rather than guessing", "[launcher][verify]")
{
    // Four states, not two. Read as `Matched` this would report a verification that
    // did not happen; read as `Mismatched` it would fail a build over a full disk.
    // What it means is "ask again", and only a value of its own can say that.
    Testing::ScratchDirectory const scratch { "hit-verify-missing" };
    auto const& dir = scratch.Path();
    auto const present = WriteFile(dir, "present.o", "abc");

    CHECK(CompareObjectFiles(present, dir / "not-here.o").verdict == HitVerdict::Inconclusive);
    CHECK(CompareObjectFiles(dir / "not-here.o", present).verdict == HitVerdict::Inconclusive);
}

TEST_CASE("A mismatch is described, and says what was linked", "[launcher][verify]")
{
    // The message IS the product here. A wrong object that is counted and not
    // described is a number somebody has to come back and ask about -- and the second
    // half, that the fresh object was used, is what tells a reader whether the build
    // they are holding is trustworthy.
    auto const line = DescribeVerdict({ .verdict = HitVerdict::Mismatched }, "objkey-v3:abcdef");
    CHECK(line.contains("objkey-v3:abcdef"));
    CHECK(line.contains("WRONG OBJECT"));
    CHECK(line.contains("freshly compiled"));

    // Nothing to say about the two ordinary outcomes -- a line per hit would make the
    // one that matters unreadable. Including when the comparison overlooked a clock,
    // which on Windows is EVERY hit.
    CHECK(DescribeVerdict({ .verdict = HitVerdict::NotChecked }, "k").empty());
    CHECK(DescribeVerdict({ .verdict = HitVerdict::Matched }, "k").empty());
    CHECK(DescribeVerdict({ .verdict = HitVerdict::Matched, .detail = "the compiler's timestamp" }, "k").empty());

    // And the inconclusive one is described too, because it is not a pass.
    CHECK_FALSE(DescribeVerdict({ .verdict = HitVerdict::Inconclusive }, "objkey-v3:abcdef").empty());
}

TEST_CASE("What differed is carried into the line, not left to be asked about", "[launcher][verify]")
{
    // `.debug$S` and `.text$mn` mean a foreign build path and stale code, and those
    // are acted on differently -- so a mismatch naming neither makes an operator come
    // back and ask a question the comparison had already answered.
    auto const line =
        DescribeVerdict({ .verdict = HitVerdict::Mismatched, .detail = "3 differing byte(s), in .text$mn" }, "k");
    CHECK(line.contains(".text$mn"));
    CHECK(line.contains("WRONG OBJECT"));
}

TEST_CASE("A toolchain that cannot be compared is not reported as a wrong object", "[launcher][verify]")
{
    // The state that must never read as a finding. An operator who takes "cannot
    // verify" for "your cache is broken" switches verification off, and the class it
    // guards goes invisible again -- which is #493 restated one level up.
    auto const line = DescribeVerdict({ .verdict = HitVerdict::Unsupported, .detail = "some format" }, "objkey-v3:abcdef");
    CHECK_FALSE(line.empty());
    CHECK(line.contains("objkey-v3:abcdef"));
    CHECK(line.contains("cannot verify"));
    CHECK_FALSE(line.contains("WRONG OBJECT"));
}
