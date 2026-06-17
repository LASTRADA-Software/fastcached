// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/ConfigMerge.hpp>

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace FastCache
{

namespace
{

    /// Forward `cli.<src>` into `dst.<field>` when the explicit-bit
    /// `cli.*Explicit` is set. The pointer-to-member parameter shape is
    /// type-safe: the compiler enforces that the explicit-bit is `bool`,
    /// and that the source/destination fields have a common type. Adding
    /// a new flag now means ONE row in `Merge` and one new explicit-bit
    /// on CliResult — no separate if-arm.
    /// @tparam T          Field type (deduced).
    /// @param dst         Mutable destination config.
    /// @param cli         CLI result carrying the explicit-bits + source values.
    /// @param explicitBit Pointer-to-member into `CliResult` selecting the bit.
    /// @param field       Pointer-to-member into `Config` selecting the field.
    template <class T>
    void MergeField(Config& dst, CliResult const& cli, bool CliResult::* explicitBit, T Config::* field) noexcept
    {
        if (cli.*explicitBit)
            dst.*field = cli.config.*field;
    }

} // namespace

Config Merge(Config fileCfg, CliResult const& cli)
{
    // Every typed CLI flag flows through the `MergeField` helper, which
    // forwards `cli.config.field` into `fileCfg.field` if and only if
    // the matching `cli.*Explicit` bit is set. Adding a new flag is a
    // single new row + a new `*Explicit` bit on CliResult — no
    // copy-pasted if-arm that an operator can silently forget to
    // update (a class of regression this branch already had to retrofit
    // four times for serviceName/lruRecency/cpuAffinity/notifyKeyspaceEvents).
    MergeField(fileCfg, cli, &CliResult::bindAddressExplicit, &Config::bindAddress);
    MergeField(fileCfg, cli, &CliResult::portExplicit, &Config::port);
    MergeField(fileCfg, cli, &CliResult::maxMemoryBytesExplicit, &Config::maxMemoryBytes);
    MergeField(fileCfg, cli, &CliResult::logLevelExplicit, &Config::logLevel);
    MergeField(fileCfg, cli, &CliResult::serviceNameExplicit, &Config::serviceName);
    MergeField(fileCfg, cli, &CliResult::storagePathExplicit, &Config::storagePath);
    MergeField(fileCfg, cli, &CliResult::storageDurabilityExplicit, &Config::storageDurability);
    MergeField(fileCfg, cli, &CliResult::storageMaxValueBytesExplicit, &Config::storageMaxValueBytes);
    MergeField(fileCfg, cli, &CliResult::workerThreadsExplicit, &Config::workerThreads);
    MergeField(fileCfg, cli, &CliResult::storageShardsExplicit, &Config::storageShards);
    MergeField(fileCfg, cli, &CliResult::listenBacklogExplicit, &Config::listenBacklog);
    MergeField(fileCfg, cli, &CliResult::logTimestampsExplicit, &Config::logTimestamps);
    MergeField(fileCfg, cli, &CliResult::logSourceExplicit, &Config::logSource);
    MergeField(fileCfg, cli, &CliResult::logEverythingExplicit, &Config::logEverything);
    MergeField(fileCfg, cli, &CliResult::requirePassExplicit, &Config::requirePass);
    MergeField(fileCfg, cli, &CliResult::authUsernameExplicit, &Config::authUsername);
    MergeField(fileCfg, cli, &CliResult::metricsEnabledExplicit, &Config::metricsEnabled);
    MergeField(fileCfg, cli, &CliResult::metricsBindAddressExplicit, &Config::metricsBindAddress);
    MergeField(fileCfg, cli, &CliResult::metricsPortExplicit, &Config::metricsPort);
    MergeField(fileCfg, cli, &CliResult::tlsEnabledExplicit, &Config::tlsEnabled);
    MergeField(fileCfg, cli, &CliResult::tlsCertPathExplicit, &Config::tlsCertPath);
    MergeField(fileCfg, cli, &CliResult::tlsKeyPathExplicit, &Config::tlsKeyPath);
    MergeField(fileCfg, cli, &CliResult::notifyKeyspaceEventsExplicit, &Config::notifyKeyspaceEvents);
    MergeField(fileCfg, cli, &CliResult::lruRecencyExplicit, &Config::lruRecency);
    MergeField(fileCfg, cli, &CliResult::cpuAffinityExplicit, &Config::cpuAffinity);

    // Three legacy "value-comparison" shapes remain because they have no
    // explicit-bit: `configPath` and `pidfile` are non-empty when set
    // (the parser only assigns them when the flag appears on the CLI),
    // and `daemon` is a one-way OR (CLI `--daemon` flips on, no
    // `--no-daemon` flag exists today — see ConfigMerge_test.cpp's
    // daemon-flag regression for the documented surface).
    auto const& cliCfg = cli.config;
    if (!cliCfg.configPath.empty())
        fileCfg.configPath = cliCfg.configPath;
    if (cliCfg.daemon)
        fileCfg.daemon = true;
    if (!cliCfg.pidfile.empty())
        fileCfg.pidfile = cliCfg.pidfile;

    // Listener endpoints: any explicit CLI `--listen` / `--listen-tls` wins
    // wholesale over whatever the YAML file declared. We deliberately do NOT
    // append-merge — mixing partial YAML listeners with partial CLI listeners
    // would make precedence depend on declaration order and surprise operators.
    // An explicit CLI list is a *replacement* of the YAML list, mirroring the
    // single-value flags above.
    if (!cliCfg.binds.empty())
        fileCfg.binds = cliCfg.binds;
    return fileCfg;
}

std::expected<void, ConfigError> ValidateBindFlagShape(CliResult const& cli, std::span<BindConfig const> binds)
{
    if (binds.empty())
        return {};
    // Map each explicit-bit to the user-facing flag name it represents so the
    // diagnostic names the actual flag the operator typed. Data-driven row
    // table — adding a new legacy bind flag is one more entry here.
    struct LegacyFlag
    {
        bool isExplicit;
        std::string_view name;
    };
    auto const flags = std::array<LegacyFlag, 3> { {
        { .isExplicit = cli.bindAddressExplicit, .name = "--bind" },
        { .isExplicit = cli.portExplicit, .name = "--port" },
        { .isExplicit = cli.tlsEnabledExplicit, .name = "--tls" },
    } };
    for (auto const& f: flags)
    {
        if (f.isExplicit)
            return std::unexpected(ConfigError {
                .code = ConfigErrorCode::ParseError,
                .source = "listeners",
                .line = 0,
                .field = "listeners",
                .context = std::format("{} cannot be combined with --listen / --listen-tls / YAML listeners:; "
                                       "use one shape or the other, not both",
                                       f.name),
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
                .source = "listeners",
                .line = 0,
                .field = "listeners",
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
