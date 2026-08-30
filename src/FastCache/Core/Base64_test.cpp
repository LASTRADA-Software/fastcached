// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>
#include <FastCache/Core/Bytes.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ranges>
#include <string>

#include <tests/Unwrap.hpp>

using FastCache::AsBytes;
using FastCache::Base64Decode;
using FastCache::Base64Encode;
using FastCache::Testing::Unwrap;

TEST_CASE("Base64 decodes the RFC 4648 test vectors", "[core][base64]")
{
    // Every padding length, because the padding arithmetic is where this function
    // goes wrong and each length exercises a different branch of it.
    CHECK(Unwrap(Base64Decode("")).empty());
    CHECK(Unwrap(Base64Decode("Zg==")) == "f");
    CHECK(Unwrap(Base64Decode("Zm8=")) == "fo");
    CHECK(Unwrap(Base64Decode("Zm9v")) == "foo");
    CHECK(Unwrap(Base64Decode("Zm9vYg==")) == "foob");
    CHECK(Unwrap(Base64Decode("Zm9vYmE=")) == "fooba");
    CHECK(Unwrap(Base64Decode("Zm9vYmFy")) == "foobar");
}

TEST_CASE("Base64 padding applies to the last group and no other", "[core][base64]")
{
    // The bug this case exists for: applying the padding count to every group
    // drops a byte from each, which decodes a short input correctly and silently
    // truncates every longer one -- so a round-trip test written with one short
    // string passes while real credentials come back mangled.
    CHECK(Unwrap(Base64Decode("YWxwaGE6YnJhdm8tY2hhcmxpZS1kZWx0YQ==")) == "alpha:bravo-charlie-delta");
    CHECK(Unwrap(Base64Decode("OnN1cGVyLXNlY3JldC10b2tlbi12YWx1ZQ==")) == ":super-secret-token-value");
    CHECK(Unwrap(Base64Decode("MTIzNDU2Nzg5MDEyMzQ1Njc4OTA=")) == "12345678901234567890");
}

TEST_CASE("Base64 refuses what it does not understand rather than skipping it", "[core][base64]")
{
    // Skipping unknown bytes is the traditional shape of this function and is
    // wrong here for a specific reason: this decodes a credential, and a decoder
    // that quietly ignores what it cannot read maps two different inputs onto one
    // secret.
    CHECK_FALSE(Base64Decode("Zm9v!").has_value());    // outside the alphabet
    CHECK_FALSE(Base64Decode("Zm9").has_value());      // not a multiple of four
    CHECK_FALSE(Base64Decode("Zm9vYg=").has_value());  // ditto, with padding
    CHECK_FALSE(Base64Decode("Z===").has_value());     // padding where a symbol belongs
    CHECK_FALSE(Base64Decode("Zg==Zg==").has_value()); // padding in the middle
    CHECK_FALSE(Base64Decode("Zm 9v").has_value());    // whitespace is not ignored

    // The URL-safe alphabet is a different encoding, and accepting both would mean
    // two spellings of one credential.
    CHECK_FALSE(Base64Decode("--__").has_value());
}

TEST_CASE("Base64 decodes bytes that are not text", "[core][base64]")
{
    // A credential file may hold arbitrary bytes; a NUL must survive rather than
    // truncating the result.
    auto const decoded = Unwrap(Base64Decode("AAECf/8="));
    REQUIRE(decoded.size() == 5);
    CHECK(decoded[0] == '\0');
    CHECK(static_cast<unsigned char>(decoded[4]) == 0xFF);
}

TEST_CASE("Base64 refuses a non-canonical final group", "[core][base64]")
{
    // The bits a padded group cannot carry must be ZERO, and a decoder that drops
    // them instead accepts several spellings of one value -- which is the failure
    // `Base64.hpp` names: "turns two different inputs into one secret". This one
    // decodes a credential, so that is not a theoretical tidiness argument.
    //
    // Two symbols carry twelve bits and one byte consumes eight, so four are
    // spare: `Q` is 010000 and `R` is 010001, and both left `0x41` behind.
    CHECK(Unwrap(Base64Decode("QQ==")) == "A");
    CHECK_FALSE(Base64Decode("QR==").has_value());

    // Three symbols carry eighteen bits and two bytes consume sixteen, so two are
    // spare. `QUE` is the canonical spelling of "AA"; `QUF` differs only in a bit
    // no byte of the output can hold.
    CHECK(Unwrap(Base64Decode("QUE=")) == "AA");
    CHECK_FALSE(Base64Decode("QUF=").has_value());

    // An unpadded group has no spare bits, so nothing here narrows what was
    // already accepted.
    CHECK(Unwrap(Base64Decode("QUJD")) == "ABC");
}

TEST_CASE("Base64 encodes the RFC 4648 test vectors", "[core][base64]")
{
    // The same vectors the decoder is checked against, so the two are pinned to the
    // published answers rather than to each other -- an encoder and a decoder that
    // agree only with one another agree about a mistake just as happily.
    CHECK(Base64Encode(AsBytes("")).empty());
    CHECK(Base64Encode(AsBytes("f")) == "Zg==");
    CHECK(Base64Encode(AsBytes("fo")) == "Zm8=");
    CHECK(Base64Encode(AsBytes("foo")) == "Zm9v");
    CHECK(Base64Encode(AsBytes("foob")) == "Zm9vYg==");
    CHECK(Base64Encode(AsBytes("fooba")) == "Zm9vYmE=");
    CHECK(Base64Encode(AsBytes("foobar")) == "Zm9vYmFy");
}

TEST_CASE("Base64 round-trips every byte value", "[core][base64]")
{
    // A signed-lease token is arbitrary bytes -- a 32-byte HMAC tag, big-endian
    // integers -- so the high half of the byte range is not an edge case here, it is
    // most of the payload. `char` is signed on x86-64 Linux and unsigned on aarch64,
    // which is exactly the kind of difference this walks into.
    std::string all;
    for (auto const value: std::views::iota(0, 256))
        all.push_back(static_cast<char>(value));

    CHECK(Unwrap(Base64Decode(Base64Encode(AsBytes(all)))) == all);

    // And every length modulo three, because that is what decides the padding.
    for (auto const length: std::views::iota(std::size_t { 0 }, std::size_t { 9 }))
    {
        auto const prefix = all.substr(0, length);
        CHECK(Unwrap(Base64Decode(Base64Encode(AsBytes(prefix)))) == prefix);
    }
}

TEST_CASE("Base64 emits only what this project's decoder accepts", "[core][base64]")
{
    // The URL-safe alphabet is deliberately not decoded here, so emitting it would
    // produce text this build cannot read back. Checked over a payload chosen to
    // reach the two symbols that differ: 0x3E is `+` and 0x3F is `/`.
    auto const text = Base64Encode(AsBytes(std::string { "\xFB\xFF\xBF" }));
    CHECK(text.contains('+'));
    CHECK(text.contains('/'));
    CHECK_FALSE(text.contains('-'));
    CHECK_FALSE(text.contains('_'));
    CHECK(Base64Decode(text).has_value());
}
