// SPDX-License-Identifier: Apache-2.0
#include <FastCache/CompileCache/CompileValue.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
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

namespace
{
/// A compile-value frame declaring `regionCount` regions and carrying
/// `trailingBytes` bytes after the header for them to be decoded from.
[[nodiscard]] std::vector<std::byte> FrameDeclaring(std::uint32_t regionCount, std::size_t trailingBytes)
{
    std::vector<std::byte> frame;
    frame.push_back(std::byte { 1 }); // version
    for ([[maybe_unused]] auto const i: { 0, 1, 2, 3 })
        frame.push_back(std::byte { 0 }); // objectLen = 0
    for (auto const shift: { 24, 16, 8, 0 })
        frame.push_back(static_cast<std::byte>((regionCount >> shift) & 0xFFU));
    frame.insert(frame.end(), trailingBytes, std::byte { 0 });
    return frame;
}
} // namespace

TEST_CASE("DecodeCompileValue refuses a region count the frame cannot supply")
{
    // Issue #267. `regionCount` is a `u32` read off the wire, and this reserved from
    // it before a single region had been read. A region costs five wire bytes at the
    // very least -- a grammar tag and a length prefix -- so the nine-byte frame below
    // declared four billion of them and asked for roughly 172 GB. Measured rather than
    // estimated: `sizeof(TextRegion)` is 40, and 0xFFFFFFFF * 40 is 171.8e9.
    //
    // Reachable from the daemon's STORE path, so `fastcached` itself was exposed and
    // not only the compile fleet, and from a worker's reply to the launcher. Run under
    // a 2 GiB address-space cap, the pre-fix decoder aborts on `std::bad_alloc`.
    auto const frame = FrameDeclaring(0xFFFFFFFFU, 0);
    REQUIRE(frame.size() == 9);

    auto const decoded = DecodeCompileValue(frame);
    // The refusal itself is the assertion, not merely "it did not crash": a decoder
    // that survived by luck would pass a crash test and still be reserving.
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error().code == ProtocolErrorCode::MalformedFrame);
    CHECK(decoded.error().context.contains("region count"));
}

TEST_CASE("DecodeCompileValue bounds a region count by the bytes actually left")
{
    // The boundary from both sides, so the guard is neither off by one nor a constant
    // somebody picked. With `n` bytes left and five needed per region, `n / 5` regions
    // is the most the frame could possibly carry.

    // Ten bytes left could carry two regions. Three is impossible, and is refused on
    // the count alone before anything is decoded.
    auto const impossible = DecodeCompileValue(FrameDeclaring(3, 10));
    REQUIRE_FALSE(impossible.has_value());
    CHECK(impossible.error().context.contains("region count exceeds"));

    // Two is exactly achievable, and those ten zero bytes really are two regions --
    // grammar tag 0 is `ShowIncludes` and a zero length is empty text, five bytes
    // each. So the guard admits precisely what the frame can carry: it refuses
    // impossible claims, and is not merely a ceiling on large ones.
    auto const achievable = DecodeCompileValue(FrameDeclaring(2, 10));
    REQUIRE(achievable.has_value());
    CHECK(achievable->textRegions.size() == 2);
    CHECK(achievable->textRegions[0].grammar == Grammar::ShowIncludes);
    CHECK(achievable->textRegions[0].bytes.empty());

    // And zero regions with nothing left is the ordinary empty value.
    CHECK(DecodeCompileValue(FrameDeclaring(0, 0)).has_value());
}
