// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>

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

TEST_CASE("CliParser: --storage parses into Config::storagePath", "[config][cli][storage]")
{
    auto const args = std::array<char const*, 1> { "--storage=/var/lib/fastcached/cache.cow" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.storagePath == "/var/lib/fastcached/cache.cow");
}

TEST_CASE("CliParser: --storage-durability parses each mode", "[config][cli][storage]")
{
    for (auto const& [text, mode]: std::initializer_list<std::pair<char const*, FastCache::StorageDurability>> {
             { "--storage-durability=fsync", FastCache::StorageDurability::Fsync },
             { "--storage-durability=batched", FastCache::StorageDurability::Batched },
             { "--storage-durability=none", FastCache::StorageDurability::None },
         })
    {
        auto const args = std::array<char const*, 1> { text };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.storageDurability == mode);
    }
}

TEST_CASE("CliParser: --storage-durability rejects unknown values", "[config][cli][storage]")
{
    auto const args = std::array<char const*, 1> { "--storage-durability=maybe" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("CliParser: --storage-max-value parses byte-size suffixes", "[config][cli][storage]")
{
    {
        auto const args = std::array<char const*, 1> { "--storage-max-value=4m" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.storageMaxValueBytes == 4ULL * 1024U * 1024U);
    }
    {
        auto const args = std::array<char const*, 1> { "--storage-max-value=512k" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.storageMaxValueBytes == 512U * 1024U);
    }
}

TEST_CASE("CliParser: --storage-max-value rejects nonsense", "[config][cli][storage]")
{
    auto const args = std::array<char const*, 1> { "--storage-max-value=abc" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(result.error().field == "storage-max-value");
}

TEST_CASE("CliParser: --execution-model defaults to Auto", "[config][cli][execution-model]")
{
    auto const args = std::array<char const*, 0> {};
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.executionModel == FastCache::ExecutionModel::Auto);
    REQUIRE_FALSE(result->executionModelExplicit);
}

TEST_CASE("CliParser: --execution-model=auto records the explicit-set flag", "[config][cli][execution-model]")
{
    // Distinguishes "user did not pass the flag" from "user typed
    // --execution-model=auto" so a YAML value cannot shadow an
    // explicit CLI auto.
    auto const args = std::array<char const*, 1> { "--execution-model=auto" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.executionModel == FastCache::ExecutionModel::Auto);
    REQUIRE(result->executionModelExplicit);
}

TEST_CASE("CliParser: --execution-model parses each mode", "[config][cli][execution-model]")
{
    for (auto const& [text, mode]: std::initializer_list<std::pair<char const*, FastCache::ExecutionModel>> {
             { "--execution-model=auto", FastCache::ExecutionModel::Auto },
             { "--execution-model=threaded", FastCache::ExecutionModel::Threaded },
             { "--execution-model=reactor", FastCache::ExecutionModel::Reactor },
         })
    {
        auto const args = std::array<char const*, 1> { text };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.executionModel == mode);
    }
}

TEST_CASE("CliParser: --execution-model rejects unknown values", "[config][cli][execution-model]")
{
    auto const args = std::array<char const*, 1> { "--execution-model=fiber" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::OutOfRange);
    REQUIRE(result.error().field == "execution-model");
}

TEST_CASE("CliParser: --help text mentions execution-model with auto", "[config][cli][execution-model]")
{
    auto const usage = std::string { FastCache::CliUsage() };
    REQUIRE(usage.find("--execution-model") != std::string::npos);
    REQUIRE(usage.find("auto|threaded|reactor") != std::string::npos);
    REQUIRE(usage.find("--threading-model") == std::string::npos);
}
