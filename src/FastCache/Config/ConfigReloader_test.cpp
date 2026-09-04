// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/Config.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/YamlReader.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include <tests/ScratchPath.hpp>

namespace
{

/// The directory is per PROCESS and removed with it, not a fixed path shared by
/// every test binary on the machine. It used to be `temp / "fastcached-test"`,
/// which two cases writing the same stem would collide on -- and which nothing
/// ever cleaned up, so it grew without bound. Static rather than per call so the
/// several files a case writes stay together.
std::filesystem::path WriteYaml(std::string_view stem, std::string_view content)
{
    static FastCache::Testing::ScratchDirectory const scratch { "fastcached-yaml-test" };
    auto path = scratch.Path() / (std::string { stem } + ".yaml");
    std::ofstream out { path };
    out << content;
    return path;
}

} // namespace

TEST_CASE("ConfigReloader: Current() returns the initial snapshot", "[config][reload]")
{
    FastCache::Config initial { .maxMemoryBytes = 1024, .bindAddress = "127.0.0.1", .port = 11500 };
    FastCache::ConfigReloader reloader { initial, {}, {} };
    auto const snapshot = reloader.Current();
    REQUIRE(snapshot->bindAddress == "127.0.0.1");
    REQUIRE(snapshot->port == 11500);
}

TEST_CASE("ConfigReloader::Reload picks up reloadable changes", "[config][reload]")
{
    auto const path = WriteYaml("reload",
                                "bind: 127.0.0.1\n"
                                "port: 11600\n"
                                "max_memory: 1024\n"
                                "log_level: info\n");
    FastCache::Config initial { .maxMemoryBytes = 1024,
                                .bindAddress = "127.0.0.1",
                                .configPath = path.string(),
                                .port = 11600,
                                .logLevel = FastCache::LogLevel::Info };
    FastCache::ConfigReloader reloader { initial, path, {} };

    bool fired = false;
    FastCache::LogLevel newLevel { FastCache::LogLevel::Info };
    reloader.Subscribe([&](auto const& /*prev*/, auto const& next) {
        fired = true;
        newLevel = next->logLevel;
    });

    // Modify file: bump max_memory and switch log_level.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11600\nmax_memory: 4096\nlog_level: warn\n";
    }

    auto const result = reloader.Reload();
    REQUIRE(result.has_value());
    REQUIRE(fired);
    REQUIRE(newLevel == FastCache::LogLevel::Warn);
    REQUIRE(reloader.Current()->maxMemoryBytes == 4096);
}

TEST_CASE("ConfigReloader::Reload rejects changes to immutable fields", "[config][reload]")
{
    auto const path = WriteYaml("immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11700\n"
                                "max_memory: 1024\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024, .bindAddress = "127.0.0.1", .configPath = path.string(), .port = 11700
    };
    FastCache::ConfigReloader reloader { initial, path, {} };

    // Try to change the port — should be rejected.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 22000\nmax_memory: 1024\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "port");
    // Live snapshot unchanged.
    REQUIRE(reloader.Current()->port == 11700);
}

TEST_CASE("ConfigReloader::Reload rejects changes to storage_path (regression for ValidateImmutable gap)",
          "[config][reload][regression]")
{
    // Regression for finding #13 — pre-fix, ValidateImmutable only
    // checked bindAddress and port. Changing storage_path, shards,
    // durability, threads, or storage_max_value via SIGHUP silently
    // swapped Config but left the running backend on the old settings.
    auto const path = WriteYaml("storage-path-immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11710\n"
                                "max_memory: 1024\n"
                                "storage_path: /tmp/a.cow\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .bindAddress = "127.0.0.1",
        .configPath = path.string(),
        .storagePath = "/tmp/a.cow",
        .port = 11710,
    };
    FastCache::ConfigReloader reloader { initial, path, {} };

    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11710\nmax_memory: 1024\nstorage_path: /tmp/b.cow\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "storage_path");
    // Live snapshot still names a.cow.
    REQUIRE(reloader.Current()->storagePath == "/tmp/a.cow");
}

TEST_CASE("ConfigReloader::Reload rejects changes to storage_durability", "[config][reload][regression]")
{
    auto const path = WriteYaml("storage-durability-immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11720\n"
                                "max_memory: 1024\n"
                                "storage_durability: fsync\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .bindAddress = "127.0.0.1",
        .configPath = path.string(),
        .port = 11720,
        .storageDurability = FastCache::StorageDurability::Fsync,
    };
    FastCache::ConfigReloader reloader { initial, path, {} };

    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11720\nmax_memory: 1024\nstorage_durability: batched\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "storage_durability");
}

TEST_CASE("ConfigReloader::Reload rejects changes to the active expiry cycle", "[config][reload][regression]")
{
    // `ExpiryReaper` is built once, with the pacing and the ceilings it will
    // use for the life of the process, on the reactor whose loop drives it.
    // Nothing hands it new ones -- so a SIGHUP accepted here would leave
    // `reloader.Current()` reporting a sweep interval the daemon is not
    // sweeping at, which is exactly the split-brain the storage and listener
    // fields above reject.
    auto const path = WriteYaml("expiry-immutable",
                                "bind: 127.0.0.1\n"
                                "port: 11740\n"
                                "max_memory: 1024\n"
                                "active_expiry_interval_ms: 1000\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .bindAddress = "127.0.0.1",
        .configPath = path.string(),
        .port = 11740,
    };
    FastCache::ConfigReloader reloader { initial, path, {} };

    // Turning the cycle off at runtime is the change an operator would most
    // plausibly try, and the one that would look most like it had worked.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11740\nmax_memory: 1024\nactive_expiry_interval_ms: 0\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "active_expiry_interval_ms");
    REQUIRE(reloader.Current()->activeExpiryIntervalMs == FastCache::DefaultActiveExpiryIntervalMs);

    // The scan budget is its own field and its own rejection, so a change to it
    // cannot ride in unnoticed behind an unchanged interval.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "bind: 127.0.0.1\nport: 11740\nmax_memory: 1024\nactive_expiry_scan: 64\n";
    }

    auto const scanResult = reloader.Reload();
    REQUIRE_FALSE(scanResult.has_value());
    REQUIRE(scanResult.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(scanResult.error().field == "active_expiry_scan");
}

TEST_CASE("ConfigReloader::Reload rejects changes to listeners (binds vector)", "[config][reload][regression]")
{
    // Finding #7: pre-fix, ValidateImmutable did not check the new
    // Config::binds vector. A SIGHUP that added/removed/swapped a
    // `listeners:` entry was silently accepted, but main.cpp builds the
    // listener pool from `serverOpts.binds` once at start, so the new
    // listeners never came up — split-brain between reloader.Current()
    // and the live wiring.
    auto const path = WriteYaml("listeners-immutable",
                                "listeners:\n"
                                "  - { address: 127.0.0.1, port: 11730 }\n"
                                "max_memory: 1024\n");
    FastCache::Config initial {
        .maxMemoryBytes = 1024,
        .configPath = path.string(),
        .binds = { FastCache::BindConfig { .address = "127.0.0.1", .port = 11730, .tls = false } },
    };
    FastCache::ConfigReloader reloader { initial, path, {} };

    // Add a second listener via SIGHUP — must reject, not silently accept.
    {
        std::ofstream out { path, std::ios::trunc };
        out << "listeners:\n"
               "  - { address: 127.0.0.1, port: 11730 }\n"
               "  - { address: 0.0.0.0, port: 11731 }\n"
               "max_memory: 1024\n";
    }

    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
    REQUIRE(result.error().field == "listeners");
    // Live snapshot still names the single original listener.
    REQUIRE(reloader.Current()->binds.size() == 1);
    REQUIRE(reloader.Current()->binds.front().port == 11730);
}

namespace
{

/// One live-wired setting, and the single YAML line that changes it.
///
/// Every row names a setting `main.cpp` consumes exactly once at startup, so a
/// reload that accepted it would leave `reloader.Current()` describing something
/// the running daemon is not doing.
struct LiveWiredSetting
{
    std::string_view key;    ///< The YAML key, which is what the refusal must name.
    std::string_view before; ///< The value the daemon started with.
    std::string_view after;  ///< The value an operator saved over it.
};

/// Seven settings the issue measured as live-wired and unguarded, plus the one the
/// option table cannot reach at all.
///
/// The last row is the sharper case: `memory_compression_min_bytes` has no CLI
/// flag, so a guard built only from the flag table would leave it through while
/// reading as complete.
///
/// Values only, never whole lines: a row spelling its key three times can name one
/// key in `key` and a different one in the file it writes, and the case would then
/// assert against a setting it is not testing.
constexpr auto LiveWired = std::to_array<LiveWiredSetting>({
    { .key = "tls_cert", .before = "a.pem", .after = "b.pem" },
    { .key = "tls_key", .before = "a.key", .after = "b.key" },
    { .key = "metrics_bind", .before = "127.0.0.1", .after = "0.0.0.0" },
    { .key = "metrics", .before = "true", .after = "false" },
    { .key = "cpu_affinity", .before = "per-core", .after = "none" },
    { .key = "listen_backlog", .before = "511", .after = "128" },
    { .key = "storage_max_disk", .before = "1m", .after = "2m" },
    { .key = "memory_compression_min_bytes", .before = "4096", .after = "8192" },
});

} // namespace

TEST_CASE("ConfigReloader: a live-wired setting is refused and the previous value survives", "[config][reload][regression]")
{
    // **Issue #406.** `ValidateImmutable` was a hand-written ladder of ten
    // comparisons with nothing keeping it in step with the settings a file can
    // carry, so every setting below reloaded silently: the snapshot took the new
    // value while the socket, the certificate, the reactor pinning and the L1
    // codec went on being what startup built them as.
    //
    // The live snapshot is seeded by READING the `before` file rather than by
    // hand, because the daemon's reload candidate is the file ALONE -- it is never
    // re-merged with argv. Seeding any other way would leave the two disagreeing
    // on fields this case is not about, and the refusal would then name one of
    // those instead.
    for (auto const& setting: LiveWired)
    {
        INFO("setting: " << setting.key);

        auto const contents = [&setting](std::string_view value) {
            return std::format("max_memory: 1024\n{}: {}\n", setting.key, value);
        };
        auto const path = WriteYaml(std::string { setting.key }, contents(setting.before));
        auto const read = FastCache::ReadYamlConfig(path);
        REQUIRE(read.has_value());
        auto initial = *read;
        initial.configPath = path.string();
        FastCache::ConfigReloader reloader { initial, path, {} };

        bool published = false;
        reloader.Subscribe([&published](auto const& /*prev*/, auto const& /*next*/) { published = true; });

        {
            std::ofstream out { path, std::ios::trunc };
            out << contents(setting.after);
        }

        auto const result = reloader.Reload();
        CHECK_FALSE(result.has_value());
        if (!result.has_value())
        {
            CHECK(result.error().code == FastCache::ConfigErrorCode::ImmutableChanged);
            // The key the operator EDITED, not the flag they did not type.
            CHECK(result.error().field == setting.key);
        }
        // Asserted on the surviving CONFIGURATION, not only on the refusal: a check
        // that reports a refusal and publishes anyway is the same split-brain
        // wearing a diagnostic. Whole-struct, because `Config::operator==` catches a
        // field the row is not about being published too -- and because a per-row
        // predicate would be the row's own `before` value transcribed a second time,
        // which can go green while asserting something the file never said.
        CHECK(*reloader.Current() == initial);
        CHECK_FALSE(published);
    }
}

TEST_CASE("ConfigReloader: Reload with no config path returns FileNotFound", "[config][reload]")
{
    FastCache::ConfigReloader reloader { FastCache::Config {}, {}, {} };
    auto const result = reloader.Reload();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::FileNotFound);
}

TEST_CASE("ConfigReloader: a reload re-applies the command line", "[config][reload]")
{
    // **[#622](https://github.com/LASTRADA-Software/fastcached/issues/622).** The
    // candidate used to be `ReadYamlConfig(path)` and nothing else, so a reloadable
    // setting given on the command line and absent from the file was published at
    // its built-in default at the first SIGHUP. `--max-memory` is the sharp one:
    // the new value reaches `InMemoryLruStorage::Resize`, which evicts down to it,
    // and nothing in the logs attributes that to a flag nobody re-read.
    //
    // The command line is PARSED rather than assembled by hand, because what makes
    // the fix work is the provenance bit the parse records -- a `CliResult` written
    // out here could set that bit without the parser ever having agreed to.
    auto const path = WriteYaml("reload-argv", "log_level: info\n");

    std::array<char const*, 2> const argv { "--max-memory=1024", "--log-level=info" };
    auto const parsed = FastCache::ParseCli(argv);
    REQUIRE(parsed.has_value());

    FastCache::ConfigSources const sources { .cli = *parsed, .metricsPortEnv = std::nullopt };
    auto const assembled = FastCache::AssembleEffectiveConfig(path, sources);
    REQUIRE(assembled.has_value());
    REQUIRE(assembled->config.maxMemoryBytes == 1024);

    FastCache::ConfigReloader reloader { assembled->config, path, sources };
    REQUIRE(reloader.Reload().has_value());

    // The file never mentioned `max_memory`, so the flag is what decides -- at a
    // reload exactly as at the start. Under the defect this is
    // `DefaultMaxMemoryBytes()`, a fraction of host RAM, published silently. The
    // second assertion is what makes the first mean something: 1024 is below the
    // 512 MiB floor that default is clamped to, so no host can make them agree.
    CHECK(reloader.Current()->maxMemoryBytes == 1024);
    CHECK(reloader.Current()->maxMemoryBytes != FastCache::Config {}.maxMemoryBytes);
}

TEST_CASE("ConfigReloader: a reload keeps a flag typed at its own default", "[config][reload]")
{
    // The provenance half, and the input no value comparison can answer for: the
    // operator read the port off the startup line and typed it to pin it. The file
    // says something else, so under the defect the candidate came back carrying the
    // FILE's port -- `port` is not reloadable, so every subsequent reload was
    // refused by name until the file was edited to agree with a flag it should have
    // outranked.
    auto const path = WriteYaml("reload-provenance", "port: 12000\n");

    // Off the constant, never a literal: the whole point is that the typed value
    // IS the compiled-in default.
    auto const portFlag = std::format("--port={}", FastCache::DefaultPort);
    std::array<char const*, 1> const argv { portFlag.c_str() };
    auto const parsed = FastCache::ParseCli(argv);
    REQUIRE(parsed.has_value());

    FastCache::ConfigSources const sources { .cli = *parsed, .metricsPortEnv = std::nullopt };
    auto const assembled = FastCache::AssembleEffectiveConfig(path, sources);
    REQUIRE(assembled.has_value());
    REQUIRE(assembled->config.port == FastCache::DefaultPort);

    FastCache::ConfigReloader reloader { assembled->config, path, sources };
    auto const reloaded = reloader.Reload();
    // The refusal text is reported, because a bare `has_value()` failure here says
    // only that SOME row of the immutability table fired and not which.
    INFO("refusal: " << (reloaded.has_value() ? std::string {} : reloaded.error().context));
    REQUIRE(reloaded.has_value());
    CHECK(reloader.Current()->port == FastCache::DefaultPort);
}

TEST_CASE("ConfigReloader: a reload re-applies the environment fallback", "[config][reload]")
{
    // The other half of #622, and it failed the same way for the same reason: a
    // container that agrees on a port through `FASTCACHED_METRICS_PORT` and writes
    // no `metrics_port:` had every reload refused, because the candidate was built
    // from a source the environment never reached.
    auto const path = WriteYaml("reload-env", "log_level: info\n");

    FastCache::ConfigSources const sources { .cli = {}, .metricsPortEnv = std::uint16_t { 9999 } };
    auto const assembled = FastCache::AssembleEffectiveConfig(path, sources);
    REQUIRE(assembled.has_value());
    REQUIRE(assembled->config.metricsPort == 9999);

    FastCache::ConfigReloader reloader { assembled->config, path, sources };
    auto const reloaded = reloader.Reload();
    INFO("refusal: " << (reloaded.has_value() ? std::string {} : reloaded.error().context));
    REQUIRE(reloaded.has_value());
    CHECK(reloader.Current()->metricsPort == 9999);
}

TEST_CASE("ConfigReloader: a reload keeps naming the file it read", "[config][reload]")
{
    // `ReadYamlConfig` never filled `configPath`, so a published snapshot stopped
    // naming its own file at the first reload -- invisible while nothing read it
    // back, and exactly the kind of field a later subscriber would trust.
    auto const path = WriteYaml("reload-path", "log_level: info\n");
    FastCache::ConfigSources const sources {};
    auto const assembled = FastCache::AssembleEffectiveConfig(path, sources);
    REQUIRE(assembled.has_value());

    FastCache::ConfigReloader reloader { assembled->config, path, sources };
    REQUIRE(reloader.Reload().has_value());
    CHECK(reloader.Current()->configPath == path.string());
}
