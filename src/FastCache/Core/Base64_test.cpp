// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/Base64.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <tests/Unwrap.hpp>

using FastCache::Base64Decode;
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
