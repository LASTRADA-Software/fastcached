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
    REQUIRE_FALSE(result->executionModelExplicit);
    REQUIRE_FALSE(result->workerThreadsExplicit);
    REQUIRE_FALSE(result->storageShardsExplicit);
    REQUIRE_FALSE(result->listenBacklogExplicit);
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

    // Find the --execution-model flag line and its continuation ("auto: ...").
    auto flagColumn = std::size_t { 0 };
    auto continuationColumn = std::size_t { 0 };
    for (auto const line: lines)
    {
        if (line.starts_with("  --execution-model"))
            flagColumn = DescriptionColumn(line);
        else if (line.find("auto: the reactor for both in-memory") != std::string_view::npos)
            continuationColumn = line.find_first_not_of(' ');
    }

    REQUIRE(flagColumn > 0);
    REQUIRE(continuationColumn == flagColumn);
}

TEST_CASE("CliParser: plain --help carries no ANSI escapes", "[config][cli][help][color]")
{
    auto const usage = FastCache::CliUsage(FastCache::UsageColor::Plain);
    REQUIRE(usage.find('\x1b') == std::string::npos);
}

TEST_CASE("CliParser: colorized --help adds ANSI escapes but identical text", "[config][cli][help][color]")
{
    auto const plain = FastCache::CliUsage(FastCache::UsageColor::Plain);
    auto const colored = FastCache::CliUsage(FastCache::UsageColor::Colored);

    // Color escapes are present...
    REQUIRE(colored.find("\x1b[") != std::string::npos);
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
