// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("CliParser: --log-timestamps sets the value and the explicit-override flag", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--log-timestamps" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.logTimestamps);
    // The explicit flag is what lets a CLI --log-timestamps override the YAML
    // value in BOTH directions (the merge in main.cpp), unlike the old one-way OR.
    REQUIRE(result->logTimestampsExplicit);
}

TEST_CASE("CliParser: --log-timestamps absent leaves the explicit-override flag clear", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--port=11211" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->logTimestampsExplicit);
    REQUIRE_FALSE(result->config.logTimestamps);
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

TEST_CASE("CliParser: --lru-mode parses each policy and defaults to approximate", "[config][cli]")
{
    // Default (flag absent) is Approximate, and the explicit-bit is clear.
    auto const def = FastCache::ParseCli(std::span<char const* const> {});
    REQUIRE(def.has_value());
    REQUIRE(def->config.lruRecency == FastCache::LruRecency::Approximate);
    REQUIRE_FALSE(def->lruRecencyExplicit);

    for (auto const& [text, mode]: std::initializer_list<std::pair<char const*, FastCache::LruRecency>> {
             { "--lru-mode=approximate", FastCache::LruRecency::Approximate },
             { "--lru-mode=strict", FastCache::LruRecency::Strict },
         })
    {
        auto const args = std::array<char const*, 1> { text };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.lruRecency == mode);
        // Whenever the flag was typed, the explicit-bit must be set so the
        // CLI value wins over a YAML default in Merge — regression for the
        // silent-drop bug that motivated this fix.
        REQUIRE(result->lruRecencyExplicit);
    }
}

TEST_CASE("CliParser: --lru-mode rejects unknown values", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--lru-mode=sloppy" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::OutOfRange);
}

TEST_CASE("CliParser: --cpu-affinity parses each policy and defaults to per-core", "[config][cli]")
{
    auto const def = FastCache::ParseCli(std::span<char const* const> {});
    REQUIRE(def.has_value());
    REQUIRE(def->config.cpuAffinity == FastCache::CpuAffinity::PerCore);
    REQUIRE_FALSE(def->cpuAffinityExplicit);

    for (auto const& [text, mode]: std::initializer_list<std::pair<char const*, FastCache::CpuAffinity>> {
             { "--cpu-affinity=none", FastCache::CpuAffinity::None },
             { "--cpu-affinity=per-core", FastCache::CpuAffinity::PerCore },
         })
    {
        auto const args = std::array<char const*, 1> { text };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.cpuAffinity == mode);
        // Same explicit-bit contract as --lru-mode: regression guard against
        // YAML silently shadowing a typed `--cpu-affinity=per-core` when the
        // value equals the default.
        REQUIRE(result->cpuAffinityExplicit);
    }
}

TEST_CASE("CliParser: --cpu-affinity rejects unknown values", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--cpu-affinity=numa" };
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

TEST_CASE("CliParser: --execution-model is no longer a recognised flag", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--execution-model=reactor" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CliParser: --threads=0 records the explicit-set flag (regression for Merge default-collision)",
          "[config][cli][regression]")
{
    // Regression for finding #14 — `--threads=0` happens to equal the
    // field's default, and the old Merge gated on value comparison.
    // The fix tracks "user typed this flag" per-flag so YAML's
    // `threads: 8` cannot shadow an explicit `--threads=0` (auto).
    auto const args = std::array<char const*, 1> { "--threads=0" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.workerThreads == 0U);
    REQUIRE(result->workerThreadsExplicit);
}

TEST_CASE("CliParser: --storage-shards=0 records the explicit-set flag", "[config][cli][regression]")
{
    auto const args = std::array<char const*, 1> { "--storage-shards=0" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.storageShards == 0U);
    REQUIRE(result->storageShardsExplicit);
}

TEST_CASE("CliParser: --listen-backlog sets the value and records the explicit-set flag", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--listen-backlog=1024" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.listenBacklog == 1024);
    REQUIRE(result->listenBacklogExplicit);
}

TEST_CASE("CliParser: --listen-backlog rejects out-of-range values", "[config][cli]")
{
    auto const zero = std::array<char const*, 1> { "--listen-backlog=0" };
    REQUIRE_FALSE(FastCache::ParseCli(std::span<char const* const> { zero }).has_value());

    auto const tooBig = std::array<char const*, 1> { "--listen-backlog=70000" };
    REQUIRE_FALSE(FastCache::ParseCli(std::span<char const* const> { tooBig }).has_value());
}

TEST_CASE("CliParser: --storage-durability=batched records the explicit-set flag", "[config][cli][regression]")
{
    // batched is the field's default; absent an explicit-tracker the
    // Merge step couldn't distinguish "user typed batched" from "flag
    // absent", so a YAML override of `none` would silently win.
    auto const args = std::array<char const*, 1> { "--storage-durability=batched" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.storageDurability == FastCache::StorageDurability::Batched);
    REQUIRE(result->storageDurabilityExplicit);
}

TEST_CASE("CliParser: omitting all flags leaves every explicit-tracker false", "[config][cli][regression]")
{
    auto const args = std::array<char const*, 0> {};
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->bindAddressExplicit);
    REQUIRE_FALSE(result->portExplicit);
    REQUIRE_FALSE(result->maxMemoryBytesExplicit);
    REQUIRE_FALSE(result->logLevelExplicit);
    REQUIRE_FALSE(result->storagePathExplicit);
    REQUIRE_FALSE(result->storageDurabilityExplicit);
    REQUIRE_FALSE(result->storageMaxValueBytesExplicit);
    REQUIRE_FALSE(result->workerThreadsExplicit);
    REQUIRE_FALSE(result->storageShardsExplicit);
    REQUIRE_FALSE(result->listenBacklogExplicit);
    REQUIRE_FALSE(result->lruRecencyExplicit);
    REQUIRE_FALSE(result->cpuAffinityExplicit);
}

TEST_CASE("CliParser: --bind sets the address and records the explicit-set flag", "[config][cli][bind]")
{
    auto const args = std::array<char const*, 1> { "--bind=192.168.1.5" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.bindAddress == "192.168.1.5");
    REQUIRE(result->bindAddressExplicit);
}

TEST_CASE("CliParser: --bind accepts IPv6 literals and hostnames verbatim (resolved at bind time)", "[config][cli][bind]")
{
    SECTION("IPv6 literal via =")
    {
        auto const args = std::array<char const*, 1> { "--bind=::1" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.bindAddress == "::1");
    }
    SECTION("hostname via separate argv element")
    {
        auto const args = std::array<char const*, 2> { "--bind", "localhost" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE(result.has_value());
        REQUIRE(result->config.bindAddress == "localhost");
        REQUIRE(result->bindAddressExplicit);
    }
}

TEST_CASE("CliParser: --bind rejects syntactically invalid addresses", "[config][cli][bind]")
{
    SECTION("empty value")
    {
        auto const args = std::array<char const*, 1> { "--bind=" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
        REQUIRE(result.error().field == "bind");
    }
    SECTION("embedded whitespace")
    {
        auto const args = std::array<char const*, 1> { "--bind=host with spaces" };
        auto const result = FastCache::ParseCli(std::span<char const* const> { args });
        REQUIRE_FALSE(result.has_value());
        REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
        REQUIRE(result.error().field == "bind");
    }
}

TEST_CASE("CliParser: --help text does not mention the removed execution-model flag", "[config][cli]")
{
    auto const usage = std::string { FastCache::CliUsage() };
    REQUIRE(!usage.contains("--execution-model"));
    REQUIRE(!usage.contains("--threading-model"));
}

namespace
{
/// Split `text` into '\n'-separated lines (the trailing segment is kept).
[[nodiscard]] std::vector<std::string_view> SplitLines(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (true)
    {
        auto const newline = text.find('\n');
        if (newline == std::string_view::npos)
        {
            lines.push_back(text);
            return lines;
        }
        lines.push_back(text.substr(0, newline));
        text.remove_prefix(newline + 1);
    }
}

/// Column (0-based) at which the description text begins on an option line:
/// the first non-space character following the run of 2+ gap spaces after
/// the flag. Internal single spaces inside a flag (e.g. "--help, -h") do not
/// count as the gap.
[[nodiscard]] std::size_t DescriptionColumn(std::string_view line)
{
    auto i = std::size_t { 2 }; // past the leading "  " indent
    while (i + 1 < line.size() && !(line[i] == ' ' && line[i + 1] == ' '))
        ++i;
    while (i < line.size() && line[i] == ' ')
        ++i;
    return i;
}

/// Remove ANSI SGR escape sequences ("\x1b[...m") from `text`.
[[nodiscard]] std::string StripAnsi(std::string_view text)
{
    std::string out;
    auto i = std::size_t { 0 };
    while (i < text.size())
    {
        if (text[i] == '\x1b')
        {
            while (i < text.size() && text[i] != 'm')
                ++i;
            if (i < text.size())
                ++i; // consume the terminating 'm'
        }
        else
        {
            out += text[i];
            ++i;
        }
    }
    return out;
}
} // namespace

TEST_CASE("CliParser: --help option descriptions share one aligned column", "[config][cli][help]")
{
    auto const usage = FastCache::CliUsage();
    auto expectedColumn = std::size_t { 0 };
    auto optionLines = std::size_t { 0 };

    for (auto const line: SplitLines(usage))
    {
        if (!line.starts_with("  --"))
            continue;
        auto const column = DescriptionColumn(line);
        if (optionLines == 0)
            expectedColumn = column;
        else
            REQUIRE(column == expectedColumn);
        ++optionLines;
    }

    // Every documented flag must have lined up against the same column.
    REQUIRE(optionLines >= 14);
    REQUIRE(expectedColumn > 0);
}

TEST_CASE("CliParser: --help wraps continuation lines to the description column", "[config][cli][help]")
{
    auto const usage = FastCache::CliUsage();
    auto const lines = SplitLines(usage);

    // Find the --lru-mode flag line and its continuation ("approximate: ...").
    auto flagColumn = std::size_t { 0 };
    auto continuationColumn = std::size_t { 0 };
    for (auto const line: lines)
    {
        if (line.starts_with("  --lru-mode"))
            flagColumn = DescriptionColumn(line);
        else if (line.contains("approximate: same-shard reads run concurrently"))
            continuationColumn = line.find_first_not_of(' ');
    }

    REQUIRE(flagColumn > 0);
    REQUIRE(continuationColumn == flagColumn);
}

TEST_CASE("CliParser: plain --help carries no ANSI escapes", "[config][cli][help][color]")
{
    auto const usage = FastCache::CliUsage(FastCache::UsageColor::Plain);
    REQUIRE(!usage.contains('\x1b'));
}

TEST_CASE("CliParser: colorized --help adds ANSI escapes but identical text", "[config][cli][help][color]")
{
    auto const plain = FastCache::CliUsage(FastCache::UsageColor::Plain);
    auto const colored = FastCache::CliUsage(FastCache::UsageColor::Colored);

    // Color escapes are present...
    REQUIRE(colored.contains("\x1b["));
    REQUIRE(colored.size() > plain.size());
    // ...and stripping them recovers exactly the plain layout (so color never
    // disturbs alignment).
    REQUIRE(StripAnsi(colored) == plain);
}

TEST_CASE("CliParser: --install-service selects the install outcome and keeps parsing flags", "[config][cli][service]")
{
    auto const args = std::array<char const*, 3> { "--install-service", "--port=6000", "--service-name=Foo" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->outcome == FastCache::CliOutcome::InstallService);
    // Flags after --install-service still land in the config that gets baked
    // into the service command line.
    REQUIRE(result->config.port == 6000U);
    REQUIRE(result->config.serviceName == "Foo");
}

TEST_CASE("CliParser: --uninstall-service selects the uninstall outcome", "[config][cli][service]")
{
    auto const args = std::array<char const*, 1> { "--uninstall-service" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->outcome == FastCache::CliOutcome::UninstallService);
}

TEST_CASE("CliParser: --requirepass / --auth-username are captured", "[config][cli][auth]")
{
    auto const args = std::array<char const*, 2> { "--requirepass=s3cr3t", "--auth-username=alice" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.requirePass == "s3cr3t");
    REQUIRE(result->config.authUsername == "alice");
    REQUIRE(result->requirePassExplicit);
    REQUIRE(result->authUsernameExplicit);
}

TEST_CASE("CliParser: --metrics enables the endpoint with bind/port", "[config][cli][metrics]")
{
    auto const args = std::array<char const*, 3> { "--metrics", "--metrics-bind=0.0.0.0", "--metrics-port=9300" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.metricsEnabled);
    REQUIRE(result->config.metricsBindAddress == "0.0.0.0");
    REQUIRE(result->config.metricsPort == 9300U);
}

TEST_CASE("CliParser: --tls captures cert and key paths", "[config][cli][tls]")
{
    auto const args = std::array<char const*, 3> { "--tls", "--tls-cert=/c.pem", "--tls-key=/k.pem" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.tlsEnabled);
    REQUIRE(result->config.tlsCertPath == "/c.pem");
    REQUIRE(result->config.tlsKeyPath == "/k.pem");
}

TEST_CASE("CliParser: --healthcheck selects the health-check outcome", "[config][cli]")
{
    auto const args = std::array<char const*, 1> { "--healthcheck" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->outcome == FastCache::CliOutcome::HealthCheck);
}

TEST_CASE("CliParser: --listen and --listen-tls populate cfg.binds in order", "[config][cli][bind]")
{
    auto const args = std::array<char const*, 2> { "--listen=127.0.0.1:11211", "--listen-tls=0.0.0.0:6380" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.binds.size() == 2);
    REQUIRE(result->config.binds[0].address == "127.0.0.1");
    REQUIRE(result->config.binds[0].port == 11211U);
    REQUIRE_FALSE(result->config.binds[0].tls);
    REQUIRE(result->config.binds[1].address == "0.0.0.0");
    REQUIRE(result->config.binds[1].port == 6380U);
    REQUIRE(result->config.binds[1].tls);
}

TEST_CASE("CliParser: --listen accepts IPv6 literal with brackets", "[config][cli][bind]")
{
    auto const args = std::array<char const*, 1> { "--listen=[::1]:11211" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.binds.size() == 1);
    REQUIRE(result->config.binds[0].address == "::1");
    REQUIRE(result->config.binds[0].port == 11211U);
}

TEST_CASE("CliParser: --listen without :port rejects with TypeMismatch", "[config][cli][bind]")
{
    auto const args = std::array<char const*, 1> { "--listen=127.0.0.1" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("CliParser: --listen rejects malformed (ipv6) spec", "[config][cli][bind]")
{
    auto const args = std::array<char const*, 1> { "--listen=[::1:11211" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("CliParser: --listen rejects unbracketed IPv6 literal with a clear diagnostic",
          "[config][cli][bind][ipv6-bracket-required]")
{
    // The inline comment in ParseListenSpec's unbracketed branch promises
    // rejection of IPv6 literals without brackets, but the implementation
    // silently split on rfind(':') — `2001:db8::1` became host=`2001:db8:`
    // (trailing colon!) + portText=`1`. The kernel later rejected the
    // bogus address with a low-level EINVAL the operator could not
    // attribute back to the missing brackets. Reject at parse time with
    // the bracket-hint diagnostic instead.
    auto const args = std::array<char const*, 1> { "--listen=2001:db8::1" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
    // Diagnostic mentions brackets so the operator knows the fix.
    REQUIRE(result.error().context.contains("brackets"));
}

TEST_CASE("CliParser: --listen-tls rejects unbracketed IPv6 literal too", "[config][cli][bind][ipv6-bracket-required]")
{
    // Symmetric guard for the --listen-tls path (shared parser).
    auto const args = std::array<char const*, 1> { "--listen-tls=2001:db8::1" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == FastCache::ConfigErrorCode::TypeMismatch);
}

TEST_CASE("CliParser: legacy --bind/--port keeps cfg.binds empty (main collapses)", "[config][cli][bind]")
{
    // The single-bind flags do NOT push into cfg.binds; main.cpp does the
    // collapse so the wire surface stays predictable for old configs.
    auto const args = std::array<char const*, 2> { "--bind=0.0.0.0", "--port=6379" };
    auto const result = FastCache::ParseCli(std::span<char const* const> { args });
    REQUIRE(result.has_value());
    REQUIRE(result->config.binds.empty());
    REQUIRE(result->config.bindAddress == "0.0.0.0");
    REQUIRE(result->config.port == 6379U);
}
