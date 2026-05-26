// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/YamlReader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace
{

std::filesystem::path WriteTempYaml(std::string_view stem, std::string_view content)
{
    auto path = std::filesystem::temp_directory_path() / "fastcached-test";
    std::filesystem::create_directories(path);
    path /= std::string { stem } + ".yaml";
    std::ofstream out { path };
    out << content;
    return path;
}

} // namespace

TEST_CASE("YamlReader: parses all recognised keys", "[config][yaml]")
{
    auto const path = WriteTempYaml("full",
                                    "bind: 0.0.0.0\n"
                                    "port: 22000\n"
                                    "max_memory: 4096\n"
                                    "log_level: debug\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->bindAddress == "0.0.0.0");
    REQUIRE(cfg->port == 22000);
    REQUIRE(cfg->maxMemoryBytes == 4096);
    REQUIRE(cfg->logLevel == FastCache::LogLevel::Debug);
}

TEST_CASE("YamlReader: missing file is reported", "[config][yaml]")
{
    auto const cfg = FastCache::ReadYamlConfig("/no/such/path/qwerty.yaml");
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::FileNotFound);
}

TEST_CASE("YamlReader: unknown keys are rejected", "[config][yaml]")
{
    auto const path = WriteTempYaml("unknown", "bogus_key: value\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::UnknownKey);
    REQUIRE(cfg.error().field == "bogus_key");
}

TEST_CASE("YamlReader: out-of-range port is rejected", "[config][yaml]")
{
    auto const path = WriteTempYaml("port", "port: 99999\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("YamlReader: empty file produces defaults", "[config][yaml]")
{
    auto const path = WriteTempYaml("empty", "");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->bindAddress == "127.0.0.1");
    REQUIRE(cfg->port == 11211);
}

TEST_CASE("YamlReader: max_memory accepts kibibyte suffix", "[config][yaml]")
{
    auto const path = WriteTempYaml("mem-k", "max_memory: 4k\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->maxMemoryBytes == 4U * 1024U);
}

TEST_CASE("YamlReader: max_memory accepts mebibyte suffix", "[config][yaml]")
{
    auto const path = WriteTempYaml("mem-m", "max_memory: 256m\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->maxMemoryBytes == 256U * 1024U * 1024U);
}

TEST_CASE("YamlReader: max_memory rejects unknown suffix", "[config][yaml]")
{
    auto const path = WriteTempYaml("mem-bad", "max_memory: 5x\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(cfg.error().field == "max_memory");
}
