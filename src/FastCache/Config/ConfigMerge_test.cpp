// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigMerge.hpp>

#include <catch2/catch_test_macros.hpp>

#include <utility>
#include <vector>

namespace
{

/// Build a CliResult with no explicit flags set — equivalent to running the
/// daemon with only a `--config` argument.
[[nodiscard]] FastCache::CliResult EmptyCli() noexcept
{
    return FastCache::CliResult {};
}

} // namespace

TEST_CASE("ConfigMerge: CLI --listen wins when YAML has no listeners", "[config][merge][bind]")
{
    FastCache::Config fileCfg {};
    fileCfg.bindAddress = "127.0.0.1";
    fileCfg.port = 11211;

    auto cli = EmptyCli();
    cli.config.binds.push_back({ .address = "0.0.0.0", .port = 6380, .tls = true });

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.binds.size() == 1);
    REQUIRE(merged.binds[0].address == "0.0.0.0");
    REQUIRE(merged.binds[0].port == 6380U);
    REQUIRE(merged.binds[0].tls);
}

TEST_CASE("ConfigMerge: YAML binds survive when CLI provides none", "[config][merge][bind]")
{
    FastCache::Config fileCfg {};
    fileCfg.binds.push_back({ .address = "10.0.0.1", .port = 11211, .tls = false });
    fileCfg.binds.push_back({ .address = "10.0.0.1", .port = 6380, .tls = true });

    auto const merged = FastCache::Merge(std::move(fileCfg), EmptyCli());
    REQUIRE(merged.binds.size() == 2);
    REQUIRE(merged.binds[0].address == "10.0.0.1");
    REQUIRE(merged.binds[0].port == 11211U);
    REQUIRE_FALSE(merged.binds[0].tls);
    REQUIRE(merged.binds[1].tls);
}

TEST_CASE("ConfigMerge: explicit CLI --listen replaces YAML listeners wholesale", "[config][merge][bind]")
{
    // YAML declared two listeners; CLI declares ONE. The CLI list replaces the
    // YAML list entirely — we do NOT append-merge (mixing partial lists would
    // make precedence order-dependent and surprise operators).
    FastCache::Config fileCfg {};
    fileCfg.binds.push_back({ .address = "10.0.0.1", .port = 11211, .tls = false });
    fileCfg.binds.push_back({ .address = "10.0.0.1", .port = 6380, .tls = true });

    auto cli = EmptyCli();
    cli.config.binds.push_back({ .address = "127.0.0.1", .port = 11211, .tls = false });

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.binds.size() == 1);
    REQUIRE(merged.binds[0].address == "127.0.0.1");
    REQUIRE_FALSE(merged.binds[0].tls);
}

TEST_CASE("ConfigMerge: bindAddress/port from CLI override YAML even with binds present", "[config][merge][bind]")
{
    // Legacy single-bind CLI flags are independent of `binds` and continue to
    // override the YAML's bindAddress/port — main.cpp synthesises a BindConfig
    // from them only when `binds` is empty.
    FastCache::Config fileCfg {};
    fileCfg.bindAddress = "10.0.0.1";
    fileCfg.port = 11211;

    auto cli = EmptyCli();
    cli.bindAddressExplicit = true;
    cli.portExplicit = true;
    cli.config.bindAddress = "127.0.0.1";
    cli.config.port = 6379;

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.bindAddress == "127.0.0.1");
    REQUIRE(merged.port == 6379U);
    REQUIRE(merged.binds.empty());
}

TEST_CASE("ConfigMerge: explicit-bit drives override even when CLI value equals default",
          "[config][merge]")
{
    // Regression guard for the original Merge design: typed flags use the
    // `explicit` bit, NOT value comparison. Setting --storage-shards=0 must
    // override a YAML shards=4 even though 0 is the default.
    FastCache::Config fileCfg {};
    fileCfg.storageShards = 4;

    auto cli = EmptyCli();
    cli.storageShardsExplicit = true;
    cli.config.storageShards = 0;

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.storageShards == 0U);
}

TEST_CASE("ValidateBinds: distinct endpoints pass", "[config][bind][validate]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "0.0.0.0", .port = 11211, .tls = false },
        { .address = "0.0.0.0", .port = 6380, .tls = true },
        { .address = "127.0.0.1", .port = 11211, .tls = false },
    };
    auto const v = FastCache::ValidateBinds(binds);
    REQUIRE(v.has_value());
}

TEST_CASE("ValidateBinds: duplicate {addr,port} pairs are rejected", "[config][bind][validate]")
{
    // The two entries differ only in `tls`. SO_REUSEPORT would let both bind
    // and the kernel would split traffic randomly across them — protocol
    // confusion the moment a plaintext client lands on the TLS listener.
    std::vector<FastCache::BindConfig> binds {
        { .address = "0.0.0.0", .port = 6379, .tls = false },
        { .address = "0.0.0.0", .port = 6379, .tls = true },
    };
    auto const v = FastCache::ValidateBinds(binds);
    REQUIRE_FALSE(v.has_value());
    REQUIRE(v.error().field == "listeners");
}

TEST_CASE("ValidateBinds: empty list is trivially valid", "[config][bind][validate]")
{
    // The empty-binds shape is rejected one layer up (main.cpp synthesises a
    // legacy fallback) — ValidateBinds itself imposes no minimum.
    std::vector<FastCache::BindConfig> binds {};
    REQUIRE(FastCache::ValidateBinds(binds).has_value());
}
