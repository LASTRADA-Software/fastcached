// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>

TEST_CASE("CliParser: --max-memory accepts plain bytes", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--max-memory=4096" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->outcome == FastCache::CliOutcome::Run);
    REQUIRE(result->config.maxMemoryBytes == 4096U);
}

TEST_CASE("CliParser: --max-memory accepts mebibyte suffix", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--max-memory=64m" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.maxMemoryBytes == 64U * 1024U * 1024U);
}

TEST_CASE("CliParser: --max-memory accepts gibibyte suffix", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--max-memory=2G" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.maxMemoryBytes == 2ULL * 1024U * 1024U * 1024U);
}

TEST_CASE("CliParser: --max-memory rejects unknown suffix", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--max-memory=5x" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(result.error().field == "max-memory");
}
