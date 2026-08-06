// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <vector>

using namespace FastCache;
using PathCanon::Grammar;

TEST_CASE("CompileValue encode/decode round-trips object blob and regions")
{
    CompileValue v;
    v.objectBlob = { std::byte { 0x00 }, std::byte { 0xFF }, std::byte { 0x10 } };
    v.textRegions.push_back({ .grammar = Grammar::ShowIncludes, .bytes = "Note: including file: <SRCROOT>/a.h\r\n" });
    v.textRegions.push_back({ .grammar = Grammar::MsvcDiagnostics, .bytes = "<SRCROOT>/a.cpp(1): warning\r\n" });

    auto const bytes = EncodeCompileValue(v);
    auto const back = DecodeCompileValue(bytes);
    REQUIRE(back.has_value());
    CHECK(back->objectBlob == v.objectBlob);
    REQUIRE(back->textRegions.size() == 2);
    CHECK(back->textRegions[0].grammar == Grammar::ShowIncludes);
    CHECK(back->textRegions[0].bytes == v.textRegions[0].bytes);
    CHECK(back->textRegions[1].grammar == Grammar::MsvcDiagnostics);
    CHECK(back->textRegions[1].bytes == v.textRegions[1].bytes);
}

TEST_CASE("CompileValue with an empty object and no regions round-trips")
{
    CompileValue const v {};
    auto const bytes = EncodeCompileValue(v);
    auto const back = DecodeCompileValue(bytes);
    REQUIRE(back.has_value());
    CHECK(back->objectBlob.empty());
    CHECK(back->textRegions.empty());
}

TEST_CASE("DecodeCompileValue rejects truncated input")
{
    auto const bytes = EncodeCompileValue({ .objectBlob = { std::byte { 1 } }, .textRegions = {} });
    auto const truncated = std::span<std::byte const> { bytes.data(), bytes.size() - 1 };
    CHECK_FALSE(DecodeCompileValue(truncated).has_value());
}

TEST_CASE("DecodeCompileValue rejects an unknown version byte")
{
    auto bytes = EncodeCompileValue({ .objectBlob = {}, .textRegions = {} });
    REQUIRE_FALSE(bytes.empty());
    bytes[0] = std::byte { 0xEE }; // corrupt version
    CHECK_FALSE(DecodeCompileValue(bytes).has_value());
}

TEST_CASE("DecodeCompileValue rejects empty input")
{
    CHECK_FALSE(DecodeCompileValue(std::span<std::byte const> {}).has_value());
}
