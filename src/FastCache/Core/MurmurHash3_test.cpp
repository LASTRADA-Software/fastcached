// SPDX-License-Identifier: Apache-2.0
//
// Conformance tests for a new component. Unlike the cases in CacheKey_test.cpp
// and DirectManifest_test.cpp, none of these fails before issue #63's fix —
// they cannot, because the thing they test did not exist. The regression cover
// for that bug lives with the key construction; what lives here is proof that
// the digest underneath it is the published algorithm and computes the same
// value everywhere.
#include <FastCache/Core/Bytes.hpp>
#include <FastCache/Core/MurmurHash3.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using FastCache::AsBytes;
using FastCache::BytesView;
using FastCache::Murmur128;
using FastCache::MurmurHash3;
using FastCache::MurmurHash3Compute;

namespace
{

/// Render a digest the way SMHasher serialises one: both words little-endian.
/// @param digest Digest to serialise.
/// @param out Sink the 16 bytes are appended to.
void AppendLittleEndian(Murmur128 digest, std::vector<std::byte>& out)
{
    for (auto const word: { digest.high, digest.low })
        for (auto const shift: std::views::iota(0, 8))
            out.push_back(static_cast<std::byte>((word >> (8 * shift)) & 0xFFULL));
}

/// A deterministic byte source. Deliberately hand-rolled rather than
/// `std::mt19937` with a distribution: the standard leaves a distribution's
/// mapping unspecified, so "fixed seed" would not mean the same bytes on
/// libstdc++, libc++ and MSVC's STL — and a cross-platform digest test whose
/// inputs differ per platform proves nothing.
class SplitMix64
{
  public:
    explicit SplitMix64(std::uint64_t seed) noexcept:
        _state { seed }
    {
    }

    /// @return The next 64 pseudorandom bits.
    [[nodiscard]] std::uint64_t Next() noexcept
    {
        _state += 0x9E37'79B9'7F4A'7C15ULL;
        auto z = _state;
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBULL;
        return z ^ (z >> 31);
    }

    /// @param length Number of bytes to produce.
    /// @return A buffer of exactly `length` pseudorandom bytes.
    [[nodiscard]] std::string NextBytes(std::size_t length)
    {
        std::string out;
        out.reserve(length);
        while (out.size() < length)
            out.push_back(static_cast<char>(Next() & 0xFFULL));
        return out;
    }

  private:
    std::uint64_t _state;
};

} // namespace

TEST_CASE("MurmurHash3 reproduces SMHasher's published verification value", "[murmurhash3]")
{
    // THE anchor. SMHasher's verification value hashes messages of every length
    // from 0 to 255 under a different seed each, concatenates the results and
    // hashes that, taking the first four bytes. 0x6384BA69 is the value published
    // for MurmurHash3_x64_128.
    //
    // Two properties fall out of this one assertion, and neither is available
    // from a fixed vector on a single string:
    //
    //  - It proves the implementation is x64_128 specifically. The sibling
    //    variant x86_128 has the same output width and a different value, so a
    //    test that merely checked "128 bits, deterministic" would accept it.
    //  - It sweeps all 256 tail lengths, so every branch of the tail assembly in
    //    Finalise is exercised. That is where a `char` signedness or endianness
    //    slip surfaces first, which is why this case is also the cross-
    //    architecture invariant: CI runs it on arm64 macOS as well as x86-64
    //    Linux and Windows, so a digest that differed by architecture is a red
    //    build rather than a silently halved cache hit rate.
    std::vector<std::byte> accumulated;
    std::vector<std::byte> key(256);
    for (auto const length: std::views::iota(std::size_t { 0 }, std::size_t { 256 }))
    {
        key[length] = static_cast<std::byte>(length & 0xFF);
        AppendLittleEndian(MurmurHash3Compute(BytesView { key.data(), length }, static_cast<std::uint32_t>(256 - length)),
                           accumulated);
    }

    auto const digest = MurmurHash3Compute(BytesView { accumulated });
    CHECK(static_cast<std::uint32_t>(digest.high & 0xFFFF'FFFFULL) == 0x6384'BA69U);
}

TEST_CASE("MurmurHash3 matches its published vectors", "[murmurhash3]")
{
    // Fixed vectors, spelled as literals so they are identical on every platform
    // or the build is red. The empty-message value is not a placeholder: the
    // x64_128 variant genuinely digests an empty message under seed 0 to all
    // zeros, which is worth pinning precisely because it looks like an
    // uninitialised result.
    struct Vector
    {
        std::string_view input;
        std::string_view expected;
    };
    static constexpr std::array vectors {
        Vector { .input = "", .expected = "00000000000000000000000000000000" },
        Vector { .input = "a", .expected = "85555565f6597889e6b53a48510e895a" },
        Vector { .input = "abc", .expected = "b4963f3f3fad78673ba2744126ca2d52" },
        Vector { .input = "123456789", .expected = "3c84645edb66cca499f8fac73a1ea105" },
        Vector { .input = "The quick brown fox jumps over the lazy dog", .expected = "e34bbc7bbc071b6c7a433ca9c49a9347" },
    };

    for (auto const& row: vectors)
    {
        INFO("input: " << row.input);
        CHECK(MurmurHash3::ToHex(MurmurHash3Compute(row.input)) == row.expected);
    }
}

TEST_CASE("MurmurHash3 streaming equals one-shot at every split", "[murmurhash3]")
{
    // This is what makes the streaming callers safe. ComputeKey feeds its fields
    // in one at a time instead of materialising a multi-megabyte blob, and
    // HashFileContents feeds a file in fixed-size chunks; both are only correct
    // if the digest depends on the concatenation and not on how it was delivered.
    // The offsets straddle the 16-byte block boundary in both directions, because
    // a carry that is mishandled only when it spans a block would survive a test
    // that split on a multiple of 16.
    SplitMix64 source { 20260821 };
    auto const message = source.NextBytes(4099);
    auto const whole = MurmurHash3Compute(std::string_view { message });

    for (auto const split: { std::size_t { 0 },
                             std::size_t { 1 },
                             std::size_t { 15 },
                             std::size_t { 16 },
                             std::size_t { 17 },
                             std::size_t { 31 },
                             std::size_t { 4095 } })
    {
        INFO("split at " << split);
        MurmurHash3 digest;
        digest.Update(std::string_view { message }.substr(0, split));
        digest.Update(std::string_view { message }.substr(split));
        CHECK(digest.Finalise() == whole);
    }
}

TEST_CASE("MurmurHash3 Finalise leaves the state extendable", "[murmurhash3]")
{
    // Finalise is const, so reading a digest mid-stream must not disturb what
    // follows. KeyDigest relies on this only implicitly today, but a Finalise
    // that folded the tail back into the state would make a digest depend on
    // whether anyone had looked at it.
    MurmurHash3 digest;
    digest.Update(std::string_view { "abc" });
    auto const peeked = digest.Finalise();
    CHECK(peeked == digest.Finalise());

    digest.Update(std::string_view { "def" });
    CHECK(digest.Finalise() == MurmurHash3Compute(std::string_view { "abcdef" }));
}

TEST_CASE("MurmurHash3 renders exactly as the format spelling it replaces", "[murmurhash3]")
{
    // ToHex uses a nibble table rather than std::format, to keep <format> out of
    // a header five translation units include. That is only safe if the two
    // agree byte for byte, since the rendering IS the cache key's wire format.
    SplitMix64 source { 7 };
    for ([[maybe_unused]] auto const round: std::views::iota(0, 64))
    {
        auto const message = source.NextBytes(37);
        auto const digest = MurmurHash3Compute(std::string_view { message });
        CHECK(MurmurHash3::ToHex(digest) == std::format("{:016x}{:016x}", digest.high, digest.low));
    }

    auto const rendered = MurmurHash3::ToHex(MurmurHash3Compute(std::string_view { "abc" }));
    CHECK(rendered.size() == MurmurHash3::HexLength);
    CHECK(rendered.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("MurmurHash3 quarters of equal-length messages vary independently", "[murmurhash3]")
{
    // The component-level twin of the guard in CacheKey_test.cpp, kept here so a
    // future change to the digest is caught without needing the launcher's suite.
    //
    // Issue #63: the construction this replaced was four CRC32C runs over one
    // blob differing only by a leading salt byte. CRC is affine, so quarter_i XOR
    // quarter_j depended only on the message LENGTH — one distinct value across
    // any number of equal-length messages, which is why matching one quarter
    // forced all four and the 128-bit key carried 32 bits.
    //
    // EQUAL LENGTH IS THE WHOLE POINT. Under the broken construction the XOR
    // varies freely once the lengths differ, so relaxing these buffers to random
    // lengths would silently defang the test.
    constexpr std::size_t Samples = 200;
    constexpr std::size_t Length = 512;

    SplitMix64 source { 63 };
    std::array<std::set<std::uint32_t>, 6> pairwiseXors;
    for ([[maybe_unused]] auto const sample: std::views::iota(std::size_t { 0 }, Samples))
    {
        auto const digest = MurmurHash3Compute(std::string_view { source.NextBytes(Length) });
        std::array<std::uint32_t, 4> const quarters {
            static_cast<std::uint32_t>(digest.high >> 32),
            static_cast<std::uint32_t>(digest.high & 0xFFFF'FFFFULL),
            static_cast<std::uint32_t>(digest.low >> 32),
            static_cast<std::uint32_t>(digest.low & 0xFFFF'FFFFULL),
        };

        std::size_t pair = 0;
        for (auto const i: std::views::iota(std::size_t { 0 }, quarters.size()))
            for (auto const j: std::views::iota(i + 1, quarters.size()))
                pairwiseXors[pair++].insert(quarters[i] ^ quarters[j]);
    }

    for (auto const index: std::views::iota(std::size_t { 0 }, pairwiseXors.size()))
    {
        INFO("quarter pair index " << index);
        auto const& seen = pairwiseXors[index];
        // The broken construction scored exactly 1 here. Anything short of
        // near-Samples means the quarters share structure.
        CHECK(seen.size() == Samples);
    }
}
