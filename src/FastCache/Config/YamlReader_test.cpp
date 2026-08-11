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

TEST_CASE("YamlReader: storage_max_value and storage_max_disk parse byte-size suffixes", "[config][yaml][storage]")
{
    auto const path = WriteTempYaml("storagecaps",
                                    "storage_path: /tmp/cache.cow\n"
                                    "storage_max_value: 256m\n"
                                    "storage_max_disk: 10g\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->storageMaxValueBytes == 256ULL * 1024U * 1024U);
    REQUIRE(cfg->storageMaxDiskBytes == 10ULL * 1024U * 1024U * 1024U);
}

TEST_CASE("YamlReader: storage_max_disk defaults to 0 (unbounded) when unset", "[config][yaml][storage]")
{
    auto const cfg = FastCache::ReadYamlConfig(WriteTempYaml("nodisk", "storage_path: /tmp/c.cow\n"));
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->storageMaxDiskBytes == 0U);
}

TEST_CASE("YamlReader: malformed storage_max_disk is rejected", "[config][yaml][storage]")
{
    auto const cfg = FastCache::ReadYamlConfig(WriteTempYaml("baddisk", "storage_max_disk: lots\n"));
    REQUIRE_FALSE(cfg.has_value());
}

TEST_CASE("YamlReader: log_source toggles the connection source prefix", "[config][yaml]")
{
    auto const on = FastCache::ReadYamlConfig(WriteTempYaml("logsrc-on", "log_source: true\n"));
    REQUIRE(on.has_value());
    REQUIRE(on->logSource);

    // Absent key keeps the default (off).
    auto const off = FastCache::ReadYamlConfig(WriteTempYaml("logsrc-off", "port: 11211\n"));
    REQUIRE(off.has_value());
    REQUIRE_FALSE(off->logSource);
}

TEST_CASE("YamlReader: log_everything toggles full command logging", "[config][yaml]")
{
    auto const on = FastCache::ReadYamlConfig(WriteTempYaml("logall-on", "log_everything: true\n"));
    REQUIRE(on.has_value());
    REQUIRE(on->logEverything);

    auto const off = FastCache::ReadYamlConfig(WriteTempYaml("logall-off", "port: 11211\n"));
    REQUIRE(off.has_value());
    REQUIRE_FALSE(off->logEverything);
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
    // Asserted against the constant, not a literal: a file with no `port:` key
    // must fall through to the compiled default, whatever that default is.
    REQUIRE(cfg->port == FastCache::DefaultPort);
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

TEST_CASE("YamlReader: execution_model is no longer accepted", "[config][yaml]")
{
    auto const path = WriteTempYaml("exec", "execution_model: reactor\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::UnknownKey);
}

TEST_CASE("YamlReader: threading_model is no longer accepted", "[config][yaml]")
{
    auto const path = WriteTempYaml("legacy", "threading_model: threaded\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::UnknownKey);
}

TEST_CASE("YamlReader: listeners sequence populates cfg.binds", "[config][yaml][listeners]")
{
    auto const path = WriteTempYaml("listeners-ok",
                                    "listeners:\n"
                                    "  - address: 0.0.0.0\n"
                                    "    port: 11211\n"
                                    "  - address: 0.0.0.0\n"
                                    "    port: 6380\n"
                                    "    tls: true\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->binds.size() == 2);
    REQUIRE(cfg->binds[0].address == "0.0.0.0");
    REQUIRE(cfg->binds[0].port == 11211U);
    REQUIRE_FALSE(cfg->binds[0].tls);
    REQUIRE(cfg->binds[1].port == 6380U);
    REQUIRE(cfg->binds[1].tls);
}

TEST_CASE("YamlReader: listeners with missing port is rejected", "[config][yaml][listeners]")
{
    auto const path = WriteTempYaml("listeners-noport",
                                    "listeners:\n"
                                    "  - address: 0.0.0.0\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().field == "listeners[0].port");
}

TEST_CASE("YamlReader: listeners with unknown field is rejected", "[config][yaml][listeners]")
{
    // A typo like `tsl` instead of `tls` must fail fast — otherwise the
    // operator silently ships a daemon that comes up plaintext.
    auto const path = WriteTempYaml("listeners-typo",
                                    "listeners:\n"
                                    "  - address: 0.0.0.0\n"
                                    "    port: 6380\n"
                                    "    tsl: true\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().field == "listeners[0].tsl");
}

TEST_CASE("YamlReader: listeners with non-sequence is rejected", "[config][yaml][listeners]")
{
    auto const path = WriteTempYaml("listeners-scalar", "listeners: 6379\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    REQUIRE(cfg.error().field == "listeners");
}

TEST_CASE("YamlReader: listeners empty sequence is rejected", "[config][yaml][listeners]")
{
    auto const path = WriteTempYaml("listeners-empty", "listeners: []\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().field == "listeners");
}

TEST_CASE("YamlReader: listeners out-of-range port is rejected", "[config][yaml][listeners]")
{
    auto const path = WriteTempYaml("listeners-bigport",
                                    "listeners:\n"
                                    "  - address: 0.0.0.0\n"
                                    "    port: 99999\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("YamlReader: ReadYamlConfigWithPresence reports bind:/port: presence", "[config][yaml][presence]")
{
    // These presence bits let main.cpp's ValidateBindFlagShape see the YAML
    // legacy single-bind triplet — without them the YAML mix of `bind:` +
    // `listeners:` silently dropped `bind:`.
    auto const both = WriteTempYaml("with-bind-port",
                                    "bind: 10.0.0.1\n"
                                    "port: 6380\n");
    auto const withBoth = FastCache::ReadYamlConfigWithPresence(both);
    REQUIRE(withBoth.has_value());
    REQUIRE(withBoth->bindAddressExplicit);
    REQUIRE(withBoth->portExplicit);
}

TEST_CASE("YamlReader: ReadYamlConfigWithPresence leaves bind/port presence clear when absent", "[config][yaml][presence]")
{
    auto const path = WriteTempYaml("no-bind", "max_memory: 4096\n");
    auto const cfg = FastCache::ReadYamlConfigWithPresence(path);
    REQUIRE(cfg.has_value());
    REQUIRE_FALSE(cfg->bindAddressExplicit);
    REQUIRE_FALSE(cfg->portExplicit);
}

TEST_CASE("YamlReader: compression keys parse", "[config][yaml][compression]")
{
    // The codec key is only valid when the codec is compiled in; the level and
    // min-bytes keys are build-independent, so a codec-less build still parses
    // them from a `compression: none` document.
    auto const* const codecLine = FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd)
                                      ? "compression: zstd\n"
                                      : "compression: none\n";
    auto const doc = std::string { codecLine } + "compression_level: 7\n" + "compression_min_bytes: 1k\n";
    auto const path = WriteTempYaml("compress", doc);
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    if (FastCache::Compression::IsAvailable(FastCache::CompressionCodec::Zstd))
        REQUIRE(cfg->compression == FastCache::CompressionCodec::Zstd);
    else
        REQUIRE(cfg->compression == FastCache::CompressionCodec::Identity);
    REQUIRE(cfg->compressionLevel == 7);
    REQUIRE(cfg->compressionMinBytes == 1024);
}

TEST_CASE("YamlReader: compression=none selects Identity", "[config][yaml][compression]")
{
    auto const path = WriteTempYaml("compress-none", "compression: none\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE(cfg.has_value());
    REQUIRE(cfg->compression == FastCache::CompressionCodec::Identity);
}

TEST_CASE("YamlReader: unknown compression codec is rejected", "[config][yaml][compression]")
{
    auto const path = WriteTempYaml("compress-bad", "compression: brotli\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("YamlReader: compression_level out of range is rejected", "[config][yaml][compression]")
{
    auto const path = WriteTempYaml("compress-lvl", "compression_level: 99\n");
    auto const cfg = FastCache::ReadYamlConfig(path);
    REQUIRE_FALSE(cfg.has_value());
    REQUIRE(cfg.error().code == FastCache::ConfigErrorCode::OutOfRange);
}
