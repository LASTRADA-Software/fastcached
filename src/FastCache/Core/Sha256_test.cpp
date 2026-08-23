// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Sha256.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;

namespace
{
/// The bytes of @p text, as a hash input.
/// @param text ASCII text.
/// @return A view of its bytes.
[[nodiscard]] std::span<std::byte const> Bytes(std::string_view text)
{
    return { reinterpret_cast<std::byte const*>(text.data()), text.size() };
}

/// Decode lowercase hex into bytes.
/// @param hex An even number of hex digits.
/// @return The bytes it spells.
[[nodiscard]] std::vector<std::byte> FromHex(std::string_view hex)
{
    constexpr std::string_view Digits = "0123456789abcdef";
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t at = 0; at + 1 < hex.size(); at += 2)
    {
        auto const hi = Digits.find(hex[at]);
        auto const lo = Digits.find(hex[at + 1]);
        out.push_back(static_cast<std::byte>((hi << 4U) | lo));
    }
    return out;
}
} // namespace

TEST_CASE("Sha256 matches the FIPS 180-4 vectors", "[core][sha256]")
{
    // The reason this algorithm is implemented here rather than invented: its
    // conformance is checkable against a published standard. These three are the
    // FIPS 180-4 / RFC 6234 examples, and the empty input is the case a hand-rolled
    // padding routine gets wrong most often -- it is all padding and no message.
    CHECK(HexDigest(Sha256::Hash({})) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HexDigest(Sha256::Hash(Bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(HexDigest(Sha256::Hash(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
          == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("Sha256 pads correctly at every block boundary", "[core][sha256]")
{
    // The padding rule has two branches -- the length either fits in this block
    // or forces another -- and the boundary between them is where a hand-written
    // implementation goes wrong. Sweeping every length from 0 to 3 blocks crosses
    // both branches and every offset within them; a single "abc" vector does not.
    //
    // Compared against the incremental path fed one byte at a time, which is the
    // other thing that can differ: a caller's chunking must not change a digest.
    for (auto const length: std::views::iota(std::size_t { 0 }, std::size_t { 200 }))
    {
        std::vector<std::byte> input(length);
        for (auto const index: std::views::iota(std::size_t { 0 }, length))
            input[index] = static_cast<std::byte>(index & 0xFFU);

        INFO("length " << length);

        Sha256 incremental;
        for (auto const& byte: input)
            incremental.Update(std::span { &byte, 1 });

        CHECK(HexDigest(incremental.Finish()) == HexDigest(Sha256::Hash(input)));
    }
}

TEST_CASE("Sha256 is unaffected by how input is chunked", "[core][sha256]")
{
    // The contract of an incremental hash, and the property `Update`'s
    // top-up-the-partial-block branch exists to provide.
    std::vector<std::byte> input(1000);
    for (auto const index: std::views::iota(std::size_t { 0 }, input.size()))
        input[index] = static_cast<std::byte>((index * 7) & 0xFFU);

    auto const oneShot = HexDigest(Sha256::Hash(input));

    for (auto const chunk: { std::size_t { 1 },
                             std::size_t { 7 },
                             std::size_t { 63 },
                             std::size_t { 64 },
                             std::size_t { 65 },
                             std::size_t { 128 },
                             std::size_t { 999 } })
    {
        INFO("chunk " << chunk);
        Sha256 hasher;
        std::span<std::byte const> remaining { input };
        while (!remaining.empty())
        {
            auto const take = std::min(chunk, remaining.size());
            hasher.Update(remaining.first(take));
            remaining = remaining.subspan(take);
        }
        CHECK(HexDigest(hasher.Finish()) == oneShot);
    }
}

TEST_CASE("HmacSha256 matches the RFC 4231 vectors", "[core][sha256][hmac]")
{
    // RFC 4231 cases 1, 2, 3 and 6. Case 6 is the one that matters most here:
    // its key is 131 bytes, longer than the 64-byte block, so it exercises the
    // "hash the key first" branch a short-key-only test never reaches.
    //
    // The long runs are CONSTRUCTED rather than written out as hex literals.
    // Typing them by hand got both case 3 and case 6 wrong the first time -- 49
    // bytes where the RFC says 50, 120 where it says 131 -- which reads as an
    // implementation failure and is not one. A vector whose input is miscounted
    // tests nothing and accuses the wrong code.
    auto const repeated = [](std::uint8_t value, std::size_t count) {
        return std::vector<std::byte>(count, static_cast<std::byte>(value));
    };

    // Case 1: 20-byte key of 0x0b, "Hi There".
    CHECK(HexDigest(HmacSha256(repeated(0x0b, 20), Bytes("Hi There")))
          == "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");

    // Case 2: key "Jefe" -- shorter than the block, so it is zero-padded.
    CHECK(HexDigest(HmacSha256(Bytes("Jefe"), Bytes("what do ya want for nothing?")))
          == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    // Case 3: 20-byte key of 0xaa, 50 bytes of 0xdd.
    CHECK(HexDigest(HmacSha256(repeated(0xaa, 20), repeated(0xdd, 50)))
          == "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");

    // Case 6: 131-byte key, longer than the block.
    CHECK(HexDigest(HmacSha256(repeated(0xaa, 131), Bytes("Test Using Larger Than Block-Size Key - Hash Key First")))
          == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");

    // Case 7: 131-byte key again, with a message longer than a block too, so the
    // inner hash spans more than one compression call.
    CHECK(HexDigest(HmacSha256(repeated(0xaa, 131),
                               Bytes("This is a test using a larger than block-size key and a larger than "
                                     "block-size data. The key needs to be hashed before being used by the HMAC "
                                     "algorithm.")))
          == "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

TEST_CASE("HmacSha256 depends on the whole key and the whole message", "[core][sha256][hmac]")
{
    // Cheap, and it catches the shape of mistake that still passes a vector: a
    // padding loop that stops early, or an inner hash fed the wrong span.
    auto const key = FromHex("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    auto const base = HmacSha256(key, Bytes("message"));

    CHECK(HexDigest(HmacSha256(key, Bytes("messagf"))) != HexDigest(base));
    CHECK(HexDigest(HmacSha256(key, Bytes("Message"))) != HexDigest(base));

    auto flipped = key;
    flipped.back() ^= std::byte { 0x01 };
    CHECK(HexDigest(HmacSha256(flipped, Bytes("message"))) != HexDigest(base));

    auto firstFlipped = key;
    firstFlipped.front() ^= std::byte { 0x01 };
    CHECK(HexDigest(HmacSha256(firstFlipped, Bytes("message"))) != HexDigest(base));
}

TEST_CASE("ConstantTimeEquals answers the same question as ==", "[core][sha256]")
{
    // The timing property itself cannot be asserted portably -- a test that timed
    // it would be flaky on a shared CI runner and would not fail when a compiler
    // optimised the guard away. What is asserted is correctness; the `volatile`
    // accumulator is what carries the timing half, and it is documented at the
    // definition rather than measured here.
    auto const a = Sha256::Hash(Bytes("one"));
    auto const b = Sha256::Hash(Bytes("two"));

    CHECK(ConstantTimeEquals(a, a));
    CHECK_FALSE(ConstantTimeEquals(a, b));

    // Differing in exactly one bit, at each end: a comparison that stopped early
    // would still get these right, so this is about correctness, not timing.
    auto lastDiffers = a;
    lastDiffers.back() ^= std::byte { 0x01 };
    CHECK_FALSE(ConstantTimeEquals(a, lastDiffers));

    auto firstDiffers = a;
    firstDiffers.front() ^= std::byte { 0x80 };
    CHECK_FALSE(ConstantTimeEquals(a, firstDiffers));
}
