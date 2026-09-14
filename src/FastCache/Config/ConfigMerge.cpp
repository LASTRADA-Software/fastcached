// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Config/YamlReader.hpp>

#include <array>
#include <expected>
#include <filesystem>
#include <format>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace FastCache
{

std::expected<EffectiveConfig, ConfigError> AssembleEffectiveConfig(std::filesystem::path const& configPath,
                                                                    ConfigSources const& sources)
{
    // One result, and every source runs the option table's appliers into it. A key
    // in the file sets the same explicit bit its flag does, so the bits on this
    // result mean "named anywhere" -- which is what the environment gate below asks.
    CliResult assembled;

    // The FILE first, because the command line runs over it. With no path there is
    // no file, and the defaults stand until argv says otherwise.
    if (!configPath.empty())
    {
        auto applied =
            ReadYamlSettings(configPath).and_then([&configPath, &assembled](std::vector<YamlSetting> const& settings) {
                return ApplyFileSettings(CliOptions(), settings, configPath, assembled);
            });
        if (!applied.has_value())
            return std::unexpected(std::move(applied).error());
    }

    // The command line SECOND, through the same appliers. A list it names REPLACES
    // the file's rather than extending it -- `--listen` over a file's `listen_tls:`
    // serves the command line's endpoints alone -- so those lists are emptied first,
    // driven off the table's `clear` column.
    std::vector<char const*> argv;
    argv.reserve(sources.args.size());
    std::ranges::transform(sources.args, std::back_inserter(argv), [](std::string const& arg) { return arg.c_str(); });
    std::span<char const* const> const tokens { argv };
    ClearListsNamedOn(CliOptions(), tokens, assembled);
    if (auto parsed = ParseOptionsInto(CliOptions(), tokens, assembled); !parsed.has_value())
        return std::unexpected(std::move(parsed).error());

    // The file the daemon is running from, so the snapshot names it. A DISCOVERED
    // file was never typed on the command line, so no applier supplied this.
    if (!configPath.empty())
        assembled.config.configPath = configPath.string();

    // The environment LAST, and only where neither of the two above named the
    // setting. Precedence is therefore CLI > file > environment > default, so a
    // stray `FASTCACHED_METRICS_PORT` cannot outrank a `metrics_port:` an operator
    // wrote -- even when they wrote the compiled-in default, which the bit the file's
    // applier set is what makes distinguishable from silence.
    if (sources.metricsPortEnv.has_value() && !assembled.metricsPortExplicit)
        assembled.config.metricsPort = *sources.metricsPortEnv;

    return EffectiveConfig { std::move(assembled) };
}

std::expected<void, ConfigError> ValidateBindFlagShape(EffectiveConfig const& effective)
{
    if (effective.Configuration().binds.empty())
        return {};
    // Each legacy setting beside the flag an operator reads in the refusal. Named
    // ANYWHERE, because a `bind:` in the file is lost to a `--listen` exactly as a
    // `--bind` is. Data-driven row table -- adding a new legacy bind setting is one
    // more entry here.
    struct LegacyFlag
    {
        bool CliResult::* named;
        std::string_view name;
    };
    constexpr auto flags = std::to_array<LegacyFlag>({
        { .named = &CliResult::bindAddressExplicit, .name = "--bind" },
        { .named = &CliResult::portExplicit, .name = "--port" },
        { .named = &CliResult::tlsEnabledExplicit, .name = "--tls" },
    });
    for (auto const& flag: flags)
    {
        if (effective.Named(flag.named))
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::ParseError,
                .source = "listen",
                .line = 0,
                .field = "listen",
                .context = std::format("{} (or its key) cannot be combined with --listen / --listen-tls "
                                       "(or listen: / listen_tls:); use one shape or the other, not both",
                                       flag.name),
            });
    }
    return {};
}

std::expected<void, ConfigError> ValidateBinds(std::span<BindConfig const> binds)
{
    std::unordered_set<std::string> seen;
    seen.reserve(binds.size());
    for (auto const& bind: binds)
    {
        // The key encodes the wire endpoint exactly: kernel-level uniqueness
        // is per {address, port} regardless of which BindConfig declared it.
        auto key = bind.address;
        key += ':';
        key += std::to_string(bind.port);
        if (!seen.insert(key).second)
        {
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::ParseError,
                .source = "listen",
                .line = 0,
                .field = "listen",
                .context = std::format("duplicate listener endpoint {}:{}", bind.address, bind.port),
            });
        }
    }
    return {};
}

std::string FormatBindSummary(std::span<BindConfig const> binds)
{
    if (binds.empty())
        return "<none>";
    std::string out;
    for (auto const& b: binds)
    {
        if (!out.empty())
            out += ", ";
        std::format_to(std::back_inserter(out), "{}:{}", b.address, b.port);
        if (b.tls)
            out += " [tls]";
    }
    return out;
}

} // namespace FastCache
