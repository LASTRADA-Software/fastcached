// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ByteSize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <string>

TEST_CASE("ParseByteSize: plain integers parse as bytes", "[config][bytesize]")
{
    REQUIRE(FastCache::ParseByteSize("0", "x").value() == 0U);
    REQUIRE(FastCache::ParseByteSize("1", "x").value() == 1U);
    REQUIRE(FastCache::ParseByteSize("1024", "x").value() == 1024U);
    REQUIRE(FastCache::ParseByteSize("67108864", "x").value() == 67108864U);
}

TEST_CASE("ParseByteSize: lowercase k/m/g multipliers", "[config][bytesize]")
{
    REQUIRE(FastCache::ParseByteSize("4k", "x").value() == 4U * 1024U);
    REQUIRE(FastCache::ParseByteSize("256m", "x").value() == 256U * 1024U * 1024U);
    REQUIRE(FastCache::ParseByteSize("2g", "x").value() == 2ULL * 1024U * 1024U * 1024U);
}

TEST_CASE("ParseByteSize: uppercase K/M/G multipliers", "[config][bytesize]")
{
    REQUIRE(FastCache::ParseByteSize("4K", "x").value() == 4U * 1024U);
    REQUIRE(FastCache::ParseByteSize("256M", "x").value() == 256U * 1024U * 1024U);
    REQUIRE(FastCache::ParseByteSize("2G", "x").value() == 2ULL * 1024U * 1024U * 1024U);
}

TEST_CASE("ParseByteSize: empty input is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("", "max-memory");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(result.error().field == "max-memory");
}

TEST_CASE("ParseByteSize: non-numeric input is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("abc", "x");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("ParseByteSize: unknown suffix is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("5x", "max-memory");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(result.error().field == "max-memory");
}

TEST_CASE("ParseByteSize: suffix without digits is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("m", "x");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("ParseByteSize: digits-followed-by-trailing-junk is TypeMismatch", "[config][bytesize]")
{
    // After stripping a numeric tail check, "12ab" has trailing 'b' as a suffix
    // candidate (unknown -> TypeMismatch). "12kx" keeps 'x' as final and is
    // unknown-suffix too. Both must fail.
    REQUIRE_FALSE(FastCache::ParseByteSize("12ab", "x").has_value());
    REQUIRE_FALSE(FastCache::ParseByteSize("12kx", "x").has_value());
}

TEST_CASE("ParseByteSize: overflow on multiply yields OutOfRange", "[config][bytesize]")
{
    // size_t::max / 2^30 ≈ 1.7e10, so 10^11 * G overflows a 64-bit size_t.
    auto const result = FastCache::ParseByteSize("99999999999G", "x");
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("ParseByteSize: size_t::max as plain bytes still parses", "[config][bytesize]")
{
    auto const maxStr = std::to_string(std::numeric_limits<std::size_t>::max());
    auto const result = FastCache::ParseByteSize(maxStr, "x");
    REQUIRE(result.has_value());
    REQUIRE(result.value() == std::numeric_limits<std::size_t>::max());
}

TEST_CASE("ParseByteSize: percent resolves against host total", "[config][bytesize]")
{
    constexpr auto HostTotal = std::size_t { 16ULL * 1024U * 1024U * 1024U }; // 16 GiB
    REQUIRE(FastCache::ParseByteSize("50%", "x", HostTotal).value() == HostTotal / 2U);
    REQUIRE(FastCache::ParseByteSize("100%", "x", HostTotal).value() == HostTotal);
    REQUIRE(FastCache::ParseByteSize("0%", "x", HostTotal).value() == 0U);
    REQUIRE(FastCache::ParseByteSize("25%", "x", HostTotal).value() == HostTotal / 4U);
}

TEST_CASE("ParseByteSize: percent > 100 yields OutOfRange", "[config][bytesize]")
{
    constexpr auto HostTotal = std::size_t { 4ULL * 1024U * 1024U * 1024U };
    auto const result = FastCache::ParseByteSize("150%", "max_memory", HostTotal);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("ParseByteSize: percent without host total is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("50%", "max_memory"); // hostTotalBytes defaults to 0
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("ParseByteSize: bare '%' is TypeMismatch", "[config][bytesize]")
{
    auto const result = FastCache::ParseByteSize("%", "x", 4096);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("FormatByteSize: zero bytes is 0B", "[config][bytesize]")
{
    REQUIRE(FastCache::FormatByteSize(0) == "0B");
}

TEST_CASE("FormatByteSize: clean multiples scale up to KB/MB/GB", "[config][bytesize]")
{
    REQUIRE(FastCache::FormatByteSize(1024) == "1K");
    REQUIRE(FastCache::FormatByteSize(4 * 1024) == "4K");
    REQUIRE(FastCache::FormatByteSize(1024 * 1024) == "1M");
    REQUIRE(FastCache::FormatByteSize(64 * 1024 * 1024) == "64M");
    REQUIRE(FastCache::FormatByteSize(2ULL * 1024 * 1024 * 1024) == "2G");
}

TEST_CASE("FormatByteSize: non-clean values fall back to bytes", "[config][bytesize]")
{
    REQUIRE(FastCache::FormatByteSize(1) == "1B");
    REQUIRE(FastCache::FormatByteSize(1025) == "1025B");
    REQUIRE(FastCache::FormatByteSize((1024 * 1024) + 1) == "1048577B");
}

TEST_CASE("Everything FormatByteSize prints, ParseByteSize accepts", "[config][bytesize]")
{
    // The two are each other's inverse or they are not worth having. Startup logs
    // print a budget with `FormatByteSize` precisely so an operator can pin it by
    // typing it back, and `4096B` was refused: `B` was the one suffix the formatter
    // emitted and the parser did not know. A percentage budget lands on a
    // non-KiB-aligned byte count almost every time, so that fallback is the common
    // case rather than the odd one.
    constexpr std::array<std::size_t, 8> Values {
        0, 1, 1023, 4096, 1685615001, 4213178368, 512ULL * 1024 * 1024, 8ULL * 1024 * 1024 * 1024
    };
    for (auto const value: Values)
    {
        auto const text = FastCache::FormatByteSize(value);
        INFO(value << " formats as " << text);
        auto const back = FastCache::ParseByteSize(text, "x");
        REQUIRE(back.has_value());
        CHECK(*back == value);
    }
}

TEST_CASE("ParseByteSize: a B suffix is plain bytes", "[config][bytesize]")
{
    CHECK(FastCache::ParseByteSize("4096B", "x").value() == 4096U);
    CHECK(FastCache::ParseByteSize("4096b", "x").value() == 4096U);
    // And it is not a multiplier anybody can confuse with the others.
    CHECK(FastCache::ParseByteSize("1B", "x").value() == 1U);
    CHECK(FastCache::ParseByteSize("1K", "x").value() == 1024U);
}
