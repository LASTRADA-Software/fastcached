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

TEST_CASE("ConfigMerge: --service-name explicit value overrides YAML even when CLI equals default",
          "[config][merge]")
{
    // serviceName previously used `cliCfg.serviceName != Config{}.serviceName`
    // (value comparison against the compiled default) instead of the
    // serviceNameExplicit bit every other field uses. Passing the default
    // value on the CLI silently fell back to the YAML value — defying the
    // documented "CLI overrides YAML" contract whenever the operator's
    // explicit choice happened to match the default.
    FastCache::Config fileCfg {};
    fileCfg.serviceName = "yaml-name";

    auto cli = EmptyCli();
    cli.serviceNameExplicit = true;
    cli.config.serviceName = FastCache::Config {}.serviceName; // == "FastCached"

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.serviceName == FastCache::Config {}.serviceName);
}

TEST_CASE("ConfigMerge: --service-name absent leaves YAML value intact",
          "[config][merge]")
{
    // Symmetric guard: without the explicit bit, the YAML value must win.
    FastCache::Config fileCfg {};
    fileCfg.serviceName = "yaml-name";

    auto const cli = EmptyCli(); // serviceNameExplicit defaults to false

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.serviceName == "yaml-name");
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

TEST_CASE("ConfigMerge: --lru-mode strict overrides YAML default approximate",
          "[config][merge][lru]")
{
    // The original Merge() shipped without lruRecency/cpuAffinity propagation,
    // so `--config foo.yaml --lru-mode=strict` silently kept the YAML default.
    FastCache::Config fileCfg {};
    fileCfg.lruRecency = FastCache::LruRecency::Approximate;

    auto cli = EmptyCli();
    cli.lruRecencyExplicit = true;
    cli.config.lruRecency = FastCache::LruRecency::Strict;

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.lruRecency == FastCache::LruRecency::Strict);
}

TEST_CASE("ConfigMerge: --cpu-affinity none overrides YAML default per-core",
          "[config][merge][cpu-affinity]")
{
    FastCache::Config fileCfg {};
    fileCfg.cpuAffinity = FastCache::CpuAffinity::PerCore;

    auto cli = EmptyCli();
    cli.cpuAffinityExplicit = true;
    cli.config.cpuAffinity = FastCache::CpuAffinity::None;

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.cpuAffinity == FastCache::CpuAffinity::None);
}

TEST_CASE("ConfigMerge: --lru-mode default-value still overrides YAML when explicit",
          "[config][merge][lru]")
{
    // Mirrors the explicit-bit-not-value-comparison invariant: `--lru-mode
    // approximate` on top of a YAML `lru_mode: strict` must win, even though
    // approximate happens to equal the field's compiled-in default.
    FastCache::Config fileCfg {};
    fileCfg.lruRecency = FastCache::LruRecency::Strict;

    auto cli = EmptyCli();
    cli.lruRecencyExplicit = true;
    cli.config.lruRecency = FastCache::LruRecency::Approximate;

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.lruRecency == FastCache::LruRecency::Approximate);
}

TEST_CASE("ValidateBindFlagShape: --bind alongside --listen is rejected",
          "[config][bind][validate]")
{
    // Pre-fix, main.cpp silently picked `binds` and discarded `bindAddress`.
    // We now fail fast and name the offending flag.
    std::vector<FastCache::BindConfig> binds {
        { .address = "1.2.3.4", .port = 6379, .tls = false },
    };
    auto cli = EmptyCli();
    cli.bindAddressExplicit = true;

    auto const result = FastCache::ValidateBindFlagShape(cli, binds);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().field == "listeners");
    REQUIRE(result.error().context.contains("--bind"));
}

TEST_CASE("ValidateBindFlagShape: --port alongside --listen is rejected",
          "[config][bind][validate]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "1.2.3.4", .port = 6379, .tls = false },
    };
    auto cli = EmptyCli();
    cli.portExplicit = true;

    auto const result = FastCache::ValidateBindFlagShape(cli, binds);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().context.contains("--port"));
}

TEST_CASE("ValidateBindFlagShape: --tls alongside --listen is rejected",
          "[config][bind][validate]")
{
    // --listen-tls is the correct shape; --tls (the legacy enable bit) mixed
    // with --listen would silently drop tlsEnabled.
    std::vector<FastCache::BindConfig> binds {
        { .address = "1.2.3.4", .port = 6379, .tls = true },
    };
    auto cli = EmptyCli();
    cli.tlsEnabledExplicit = true;

    auto const result = FastCache::ValidateBindFlagShape(cli, binds);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().context.contains("--tls"));
}

TEST_CASE("ValidateBindFlagShape: --listen alone is accepted",
          "[config][bind][validate]")
{
    // The normal multi-listener case — only --listen / --listen-tls were typed.
    std::vector<FastCache::BindConfig> binds {
        { .address = "1.2.3.4", .port = 6379, .tls = false },
    };
    auto const cli = EmptyCli();
    REQUIRE(FastCache::ValidateBindFlagShape(cli, binds).has_value());
}

TEST_CASE("ValidateBindFlagShape: empty binds + any legacy flag is accepted",
          "[config][bind][validate]")
{
    // Legacy single-bind path — binds is empty, so main.cpp synthesises from
    // bindAddress/port/tlsEnabled and nothing is silently dropped.
    std::vector<FastCache::BindConfig> binds {};
    auto cli = EmptyCli();
    cli.bindAddressExplicit = true;
    cli.portExplicit = true;
    cli.tlsEnabledExplicit = true;
    REQUIRE(FastCache::ValidateBindFlagShape(cli, binds).has_value());
}

TEST_CASE("FormatBindSummary: single plaintext bind", "[config][bind][summary]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "127.0.0.1", .port = 11211, .tls = false },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "127.0.0.1:11211");
}

TEST_CASE("FormatBindSummary: single TLS bind carries [tls] marker", "[config][bind][summary]")
{
    std::vector<FastCache::BindConfig> binds {
        { .address = "127.0.0.1", .port = 6379, .tls = true },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "127.0.0.1:6379 [tls]");
}

TEST_CASE("FormatBindSummary: two binds (plain + TLS) join with ', '",
          "[config][bind][summary]")
{
    // Regression test for the original banner bug: the daemon brought up
    // with `--listen a:1 --listen-tls b:2` used to log
    // "bind=127.0.0.1:11211" — the defaults of the unused legacy fields.
    std::vector<FastCache::BindConfig> binds {
        { .address = "10.0.0.1", .port = 11211, .tls = false },
        { .address = "10.0.0.1", .port = 6380, .tls = true },
    };
    REQUIRE(FastCache::FormatBindSummary(binds) == "10.0.0.1:11211, 10.0.0.1:6380 [tls]");
}

TEST_CASE("FormatBindSummary: empty list renders <none>", "[config][bind][summary]")
{
    // Defensive — RunReactorServer rejects empty `binds` one layer up, but
    // FormatBindSummary itself stays total.
    std::vector<FastCache::BindConfig> binds {};
    REQUIRE(FastCache::FormatBindSummary(binds) == "<none>");
}

TEST_CASE("ConfigMerge: YAML lru/cpu survive when CLI did not pass the flag",
          "[config][merge][lru][cpu-affinity]")
{
    // Negative case: without the explicit bit, the YAML value must survive
    // even if cliCfg.lruRecency happens to differ from it (it shouldn't, but
    // it must be invisible to Merge).
    FastCache::Config fileCfg {};
    fileCfg.lruRecency = FastCache::LruRecency::Strict;
    fileCfg.cpuAffinity = FastCache::CpuAffinity::None;

    auto cli = EmptyCli();
    cli.config.lruRecency = FastCache::LruRecency::Approximate; // not explicit
    cli.config.cpuAffinity = FastCache::CpuAffinity::PerCore;   // not explicit

    auto const merged = FastCache::Merge(std::move(fileCfg), cli);
    REQUIRE(merged.lruRecency == FastCache::LruRecency::Strict);
    REQUIRE(merged.cpuAffinity == FastCache::CpuAffinity::None);
}
