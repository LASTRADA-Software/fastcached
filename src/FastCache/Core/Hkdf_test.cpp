// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Hkdf.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

#include <tests/HexBytes.hpp>

using namespace FastCache;
using FastCache::Testing::FromHex;

namespace
{

/// One of RFC 5869 Appendix A's HKDF-SHA256 test cases.
struct Rfc5869Case
{
    std::string_view name;             ///< The appendix section.
    std::string_view inputKeyMaterial; ///< IKM.
    std::string_view salt;             ///< salt, possibly empty.
    std::string_view info;             ///< info, possibly empty.
    std::size_t length;                ///< L.
    std::string_view pseudoRandomKey;  ///< PRK.
    std::string_view output;           ///< OKM.
};

/// RFC 5869 A.1 to A.3, the three SHA-256 cases.
constexpr std::array Rfc5869Cases {
    Rfc5869Case {
        .name = "A.1",
        .inputKeyMaterial = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
        .salt = "000102030405060708090a0b0c",
        .info = "f0f1f2f3f4f5f6f7f8f9",
        .length = 42,
        .pseudoRandomKey = "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
        .output = "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
    },
    Rfc5869Case {
        .name = "A.2",
        .inputKeyMaterial = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                            "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                            "404142434445464748494a4b4c4d4e4f",
        .salt = "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
                "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
                "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf",
        .info = "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff",
        .length = 82,
        .pseudoRandomKey = "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
        .output = "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
                  "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
                  "cc30c58179ec3e87c14c01d5c1f3434f1d87",
    },
    Rfc5869Case {
        .name = "A.3",
        .inputKeyMaterial = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
        .salt = "",
        .info = "",
        .length = 42,
        .pseudoRandomKey = "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
        .output = "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8",
    },
};

/// @p secret as a byte vector, for comparing against a case's hex.
/// @param secret Derived key material.
/// @return Its bytes.
[[nodiscard]] std::vector<std::byte> BytesOf(SecureByteBuffer const& secret)
{
    return { secret.begin(), secret.end() };
}

} // namespace

TEST_CASE("HKDF-SHA256 matches RFC 5869 test cases A.1 to A.3", "[core][crypto][hkdf]")
{
    for (auto const& testCase: Rfc5869Cases)
    {
        CAPTURE(testCase.name);
        auto const inputKeyMaterial = FromHex(testCase.inputKeyMaterial);
        auto const salt = FromHex(testCase.salt);
        auto const info = FromHex(testCase.info);

        auto const pseudoRandomKey = HkdfSha256Extract(salt, inputKeyMaterial);
        CHECK(BytesOf(pseudoRandomKey) == FromHex(testCase.pseudoRandomKey));

        auto const expanded = HkdfSha256Expand(pseudoRandomKey, info, testCase.length);
        REQUIRE(expanded.has_value());
        CHECK(expanded.value().size() == testCase.length);
        CHECK(BytesOf(expanded.value()) == FromHex(testCase.output));

        auto const whole = HkdfSha256(salt, inputKeyMaterial, info, testCase.length);
        REQUIRE(whole.has_value());
        CHECK(BytesOf(whole.value()) == FromHex(testCase.output));
    }
}

TEST_CASE("HKDF-SHA256 output is a prefix of any longer output from the same inputs", "[core][crypto][hkdf]")
{
    // Every block depends only on the ones before it, so asking for fewer bytes must truncate
    // rather than change them -- the property that makes the last, partial block safe to cut.
    auto const pseudoRandomKey = HkdfSha256Extract(FromHex("00"), FromHex("0b0b0b0b"));
    auto const longest = HkdfSha256Expand(pseudoRandomKey, {}, HkdfSha256MaxOutputBytes);
    REQUIRE(longest.has_value());
    REQUIRE(longest.value().size() == HkdfSha256MaxOutputBytes);
    for (auto const length: { std::size_t { 1 },
                              std::size_t { 31 },
                              std::size_t { 32 },
                              std::size_t { 33 },
                              std::size_t { 100 },
                              HkdfSha256MaxOutputBytes - 1 })
    {
        CAPTURE(length);
        auto const shorter = HkdfSha256Expand(pseudoRandomKey, {}, length);
        REQUIRE(shorter.has_value());
        std::vector<std::byte> const expected(longest.value().begin(),
                                              longest.value().begin() + static_cast<std::ptrdiff_t>(length));
        CHECK(BytesOf(shorter.value()) == expected);
    }
}

TEST_CASE("HkdfSha256Expand refuses an output longer than 255 blocks", "[core][crypto][hkdf][negative]")
{
    auto const pseudoRandomKey = HkdfSha256Extract({}, FromHex("0b0b0b0b"));
    REQUIRE(HkdfSha256MaxOutputBytes == 255 * 32);
    REQUIRE(HkdfSha256Expand(pseudoRandomKey, {}, HkdfSha256MaxOutputBytes).has_value());

    auto const refused = HkdfSha256Expand(pseudoRandomKey, {}, HkdfSha256MaxOutputBytes + 1);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == CryptoError::OutputTooLong);

    auto const refusedWhole = HkdfSha256({}, FromHex("0b0b0b0b"), {}, HkdfSha256MaxOutputBytes + 1);
    REQUIRE_FALSE(refusedWhole.has_value());
    CHECK(refusedWhole.error() == CryptoError::OutputTooLong);
}

TEST_CASE("HkdfSha256Expand refuses an empty output", "[core][crypto][hkdf][negative]")
{
    auto const refused = HkdfSha256Expand(HkdfSha256Extract({}, FromHex("0b0b0b0b")), {}, 0);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == CryptoError::EmptyOutput);
}

TEST_CASE("HkdfSha256Expand refuses a pseudorandom key shorter than one digest", "[core][crypto][hkdf][negative]")
{
    std::vector<std::byte> const shortKey(HkdfSha256HashBytes - 1, std::byte { 0x07 });
    auto const refused = HkdfSha256Expand(shortKey, {}, 32);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == CryptoError::PseudoRandomKeyTooShort);
}
