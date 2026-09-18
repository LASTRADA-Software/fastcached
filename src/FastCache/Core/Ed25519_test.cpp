// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Ed25519.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tests/HexBytes.hpp>

using namespace FastCache;
using FastCache::Testing::ArrayFromHex;
using FastCache::Testing::FromHex;

namespace
{

/// One of RFC 8032's Ed25519 test vectors, spelled as the RFC prints it.
struct Rfc8032Vector
{
    std::string_view name;      ///< The RFC's own label for it.
    std::string_view seed;      ///< SECRET KEY: the 32-byte seed.
    std::string_view publicKey; ///< PUBLIC KEY.
    std::string_view message;   ///< MESSAGE, possibly empty.
    std::string_view signature; ///< SIGNATURE.
};

/// TEST 1024's message: 1023 bytes, as RFC 8032 §7.1 prints it (extracted from the RFC text and
/// checked to decode to exactly 1023 bytes, so no line was dropped at a page break).
constexpr std::string_view Test1024Message = "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
                                             "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
                                             "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
                                             "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
                                             "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
                                             "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
                                             "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
                                             "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
                                             "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
                                             "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
                                             "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
                                             "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
                                             "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
                                             "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
                                             "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
                                             "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
                                             "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
                                             "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
                                             "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
                                             "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
                                             "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
                                             "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
                                             "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
                                             "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
                                             "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
                                             "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
                                             "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
                                             "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
                                             "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
                                             "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
                                             "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
                                             "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0";

/// RFC 8032 §7.1, the five Ed25519 vectors, in the RFC's order.
constexpr std::array Rfc8032Vectors {
    Rfc8032Vector {
        .name = "TEST 1",
        .seed = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        .publicKey = "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
        .message = "",
        .signature = "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b4"
                     "6bd25bf5f0595bbe24655141438e7a100b",
    },
    Rfc8032Vector {
        .name = "TEST 2",
        .seed = "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        .publicKey = "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
        .message = "72",
        .signature = "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d"
                     "8c387b2eaeb4302aeeb00d291612bb0c00",
    },
    Rfc8032Vector {
        .name = "TEST 3",
        .seed = "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
        .publicKey = "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
        .message = "af82",
        .signature = "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6"
                     "594a7c15e9716ed28dc027beceea1ec40a",
    },
    Rfc8032Vector {
        .name = "TEST 1024",
        .seed = "f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5",
        .publicKey = "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
        .message = Test1024Message,
        .signature = "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350aa5371b1508f9f4528ecea23c436d9"
                     "4b5e8fcd4f681e30a6ac00a9704a188a03",
    },
    Rfc8032Vector {
        .name = "TEST SHA(abc)",
        .seed = "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
        .publicKey = "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
        .message = "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
                   "454d4423643ce80e2a9ac94fa54ca49f",
        .signature = "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0"
                     "303dca179c138ac17ad9bef1177331a704",
    },
};

/// The group order L = 2^252 + 27742317777372353535851937790883648493, little-endian, as
/// RFC 8032 §5.1 defines it -- the bound a signature's S must stay below.
constexpr std::string_view GroupOrderL = "edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010";

/// The key pair @p vector's seed determines. The seed's length is the RFC's, so this cannot refuse.
/// @param vector The vector.
/// @return Its key pair.
[[nodiscard]] Ed25519KeyPair KeyPairOf(Rfc8032Vector const& vector)
{
    auto const seed = FromHex(vector.seed);
    auto keyPair = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(keyPair.has_value());
    return keyPair.value();
}

/// @p signature with its scalar S replaced by S + L: the same residue modulo L, so a verifier
/// that skipped the range check would accept it as a second valid signature of the same message.
/// @param signature A signature whose S is canonical (below L).
/// @return The malleated signature.
[[nodiscard]] Ed25519Signature WithGroupOrderAdded(Ed25519Signature signature)
{
    auto const order = FromHex(GroupOrderL);
    unsigned carry = 0;
    for (auto const index: std::views::iota(std::size_t { 0 }, order.size()))
    {
        auto& byte = signature[32 + index];
        carry += std::to_integer<unsigned>(byte) + std::to_integer<unsigned>(order[index]);
        byte = static_cast<std::byte>(carry & 0xFFU);
        carry >>= 8U;
    }
    // Every RFC signature's S is far enough below 2^253 that S + L still fits in 32 bytes; a
    // carry out would mean the test built something other than S + L.
    REQUIRE(carry == 0);
    return signature;
}

} // namespace

TEST_CASE("Ed25519 matches the RFC 8032 section 7.1 vectors", "[core][crypto][ed25519]")
{
    for (auto const& vector: Rfc8032Vectors)
    {
        CAPTURE(vector.name);
        auto const keyPair = KeyPairOf(vector);
        auto const message = FromHex(vector.message);
        auto const expectedSignature = ArrayFromHex<Ed25519SignatureBytes>(vector.signature);

        CHECK(keyPair.PublicKey() == ArrayFromHex<Ed25519PublicKeyBytes>(vector.publicKey));
        CHECK(keyPair.Sign(message) == expectedSignature);
        CHECK(Ed25519Verify(keyPair.PublicKey(), message, expectedSignature));
    }
}

TEST_CASE("Ed25519 TEST 1024 carries the message length the RFC states", "[core][crypto][ed25519]")
{
    // The one vector long enough to lose a line at a page break of the RFC text, so its decoded
    // length is pinned against the RFC's own "(length 1023 bytes)" rather than trusted.
    CHECK(FromHex(Test1024Message).size() == 1023);
}

TEST_CASE("Ed25519 signing is deterministic", "[core][crypto][ed25519]")
{
    auto const keyPair = KeyPairOf(Rfc8032Vectors[2]);
    auto const message = FromHex(Rfc8032Vectors[2].message);
    CHECK(keyPair.Sign(message) == keyPair.Sign(message));
}

TEST_CASE("Ed25519KeyPair refuses a seed that is not 32 bytes", "[core][crypto][ed25519]")
{
    for (auto const length: { std::size_t { 0 }, std::size_t { 31 }, std::size_t { 33 }, std::size_t { 64 } })
    {
        CAPTURE(length);
        std::vector<std::byte> const seed(length, std::byte { 0x42 });
        auto const keyPair = Ed25519KeyPair::FromSeed(seed);
        REQUIRE_FALSE(keyPair.has_value());
        CHECK(keyPair.error() == CryptoError::WrongKeyLength);
    }
}

TEST_CASE("Ed25519KeyPair leaves the caller's seed untouched", "[core][crypto][ed25519]")
{
    // Monocypher's derivation wipes the seed it is handed; the seam hands it a copy.
    auto const seed = FromHex(Rfc8032Vectors[0].seed);
    auto const keyPair = Ed25519KeyPair::FromSeed(seed);
    REQUIRE(keyPair.has_value());
    CHECK(seed == FromHex(Rfc8032Vectors[0].seed));
}

TEST_CASE("Ed25519Verify refuses a signature with any single bit flipped", "[core][crypto][ed25519][negative]")
{
    auto const& vector = Rfc8032Vectors[2];
    auto const publicKey = ArrayFromHex<Ed25519PublicKeyBytes>(vector.publicKey);
    auto const message = FromHex(vector.message);
    auto const signature = ArrayFromHex<Ed25519SignatureBytes>(vector.signature);
    REQUIRE(Ed25519Verify(publicKey, message, signature));

    // Every one of the 512 bits, R and S alike: a flip in R names a different point (or none), a
    // flip in S changes the scalar or pushes it past L.
    std::size_t accepted = 0;
    for (auto const bit: std::views::iota(std::size_t { 0 }, Ed25519SignatureBytes * 8))
    {
        auto tampered = signature;
        tampered[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
        if (Ed25519Verify(publicKey, message, tampered))
            ++accepted;
    }
    CHECK(accepted == 0);
}

TEST_CASE("Ed25519Verify refuses a message with any single bit flipped", "[core][crypto][ed25519][negative]")
{
    for (auto const& vector: { Rfc8032Vectors[2], Rfc8032Vectors[3] })
    {
        CAPTURE(vector.name);
        auto const publicKey = ArrayFromHex<Ed25519PublicKeyBytes>(vector.publicKey);
        auto const message = FromHex(vector.message);
        auto const signature = ArrayFromHex<Ed25519SignatureBytes>(vector.signature);
        REQUIRE(Ed25519Verify(publicKey, message, signature));

        // TEST 3's every bit, and a spread across TEST 1024's first, middle and last bytes.
        auto const bitCount = message.size() * 8;
        std::vector<std::size_t> bits { 0, 7, bitCount / 2, bitCount - 1 };
        if (bitCount <= 16)
        {
            bits.clear();
            std::ranges::copy(std::views::iota(std::size_t { 0 }, bitCount), std::back_inserter(bits));
        }

        std::size_t accepted = 0;
        for (auto const bit: bits)
        {
            auto tampered = message;
            tampered[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            if (Ed25519Verify(publicKey, tampered, signature))
                ++accepted;
        }
        CHECK(accepted == 0);
    }
}

TEST_CASE("Ed25519Verify refuses a signature checked against the wrong public key", "[core][crypto][ed25519][negative]")
{
    auto const& signer = Rfc8032Vectors[1];
    auto const message = FromHex(signer.message);
    auto const signature = ArrayFromHex<Ed25519SignatureBytes>(signer.signature);
    REQUIRE(Ed25519Verify(ArrayFromHex<Ed25519PublicKeyBytes>(signer.publicKey), message, signature));

    for (auto const& other: Rfc8032Vectors)
    {
        if (other.name == signer.name)
            continue;
        CAPTURE(other.name);
        CHECK_FALSE(Ed25519Verify(ArrayFromHex<Ed25519PublicKeyBytes>(other.publicKey), message, signature));
    }
}

TEST_CASE("Ed25519Verify refuses a non-canonical S at or above the group order", "[core][crypto][ed25519][negative]")
{
    // S + L is the same scalar modulo L, so the verification EQUATION still holds for it; only the
    // range check refuses it. That check is what stops one valid signature being turned into a
    // second, and the reason this case exists.
    for (auto const& vector: Rfc8032Vectors)
    {
        CAPTURE(vector.name);
        auto const publicKey = ArrayFromHex<Ed25519PublicKeyBytes>(vector.publicKey);
        auto const message = FromHex(vector.message);
        auto const signature = ArrayFromHex<Ed25519SignatureBytes>(vector.signature);
        REQUIRE(Ed25519Verify(publicKey, message, signature));

        auto const malleated = WithGroupOrderAdded(signature);
        REQUIRE(malleated != signature);
        CHECK_FALSE(Ed25519Verify(publicKey, message, malleated));
    }
}

TEST_CASE("An Ed25519 public key is spelled as 43 characters of unpadded base64url", "[core][crypto][ed25519]")
{
    // The expected strings were computed by a DIFFERENT implementation -- Python's
    // `base64.urlsafe_b64encode` with the padding stripped -- so the encoder is pinned to an
    // answer it did not produce. TEST 2's key opens with 0xFC, which reaches `_`, one of the two
    // symbols the URL-safe alphabet exists for.
    auto const test1 =
        ArrayFromHex<Ed25519PublicKeyBytes>("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    auto const test2 =
        ArrayFromHex<Ed25519PublicKeyBytes>("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025");
    constexpr std::string_view Test1Text = "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo";
    constexpr std::string_view Test2Text = "_FHNjmIYoaONpH7QAjDwWAgW7RO6MwOsXeuRFUiQgCU";

    CHECK(FormatEd25519PublicKey(test1) == Test1Text);
    CHECK(FormatEd25519PublicKey(test2) == Test2Text);
    CHECK(FormatEd25519PublicKey(test1).size() == Ed25519PublicKeyTextLength);

    auto const parsed1 = ParseEd25519PublicKey(Test1Text);
    auto const parsed2 = ParseEd25519PublicKey(Test2Text);
    REQUIRE(parsed1.has_value());
    REQUIRE(parsed2.has_value());
    CHECK(*parsed1 == test1);
    CHECK(*parsed2 == test2);
}

TEST_CASE("A public key's text is refused by what is wrong with it", "[core][crypto][ed25519][negative]")
{
    constexpr std::string_view Whole = "11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo";
    auto const faultOf = [](std::string_view text) {
        auto const parsed = ParseEd25519PublicKey(text);
        REQUIRE_FALSE(parsed.has_value());
        return parsed.error();
    };

    // Cut short and run long -- the second is also what padding looks like, which a key's
    // spelling never carries.
    CHECK(faultOf(Whole.substr(0, 42)) == PublicKeyTextFault::WrongLength);
    CHECK(faultOf(std::string { Whole } + "=") == PublicKeyTextFault::WrongLength);
    CHECK(faultOf("") == PublicKeyTextFault::WrongLength);

    // The standard alphabet's spelling of the same key: `_` becomes `/`.
    auto standard = std::string { Whole };
    std::ranges::replace(standard, '_', '/');
    CHECK(faultOf(standard) == PublicKeyTextFault::NotBase64Url);

    // A last character carrying a bit no 32-byte key has: `o` is 101000 and `p` is 101001, and
    // a decoder that dropped the two spare bits would read both as one key.
    auto nonCanonical = std::string { Whole };
    nonCanonical.back() = 'p';
    CHECK(faultOf(nonCanonical) == PublicKeyTextFault::NotBase64Url);

    // Every fault has a sentence, and the sentence says what a key looks like.
    CHECK(DescribePublicKeyTextFault(PublicKeyTextFault::WrongLength).contains("43"));
    CHECK(DescribePublicKeyTextFault(PublicKeyTextFault::NotBase64Url).contains("base64url"));
}
