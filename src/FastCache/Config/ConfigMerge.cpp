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

Config Merge(Config fileCfg, CliResult const& cli)
{
    auto const& cliCfg = cli.config;
    if (cli.bindAddressExplicit)
        fileCfg.bindAddress = cliCfg.bindAddress;
    if (cli.portExplicit)
        fileCfg.port = cliCfg.port;
    if (cli.maxMemoryBytesExplicit)
        fileCfg.maxMemoryBytes = cliCfg.maxMemoryBytes;
    if (cli.logLevelExplicit)
        fileCfg.logLevel = cliCfg.logLevel;
    if (!cliCfg.configPath.empty())
        fileCfg.configPath = cliCfg.configPath;
    if (cliCfg.daemon)
        fileCfg.daemon = true;
    if (!cliCfg.pidfile.empty())
        fileCfg.pidfile = cliCfg.pidfile;
    if (cliCfg.serviceName != Config {}.serviceName)
        fileCfg.serviceName = cliCfg.serviceName;
    if (cli.storagePathExplicit)
        fileCfg.storagePath = cliCfg.storagePath;
    if (cli.storageDurabilityExplicit)
        fileCfg.storageDurability = cliCfg.storageDurability;
    if (cli.storageMaxValueBytesExplicit)
        fileCfg.storageMaxValueBytes = cliCfg.storageMaxValueBytes;
    if (cli.workerThreadsExplicit)
        fileCfg.workerThreads = cliCfg.workerThreads;
    if (cli.storageShardsExplicit)
        fileCfg.storageShards = cliCfg.storageShards;
    if (cli.listenBacklogExplicit)
        fileCfg.listenBacklog = cliCfg.listenBacklog;
    if (cli.logTimestampsExplicit)
        fileCfg.logTimestamps = cliCfg.logTimestamps;
    if (cli.requirePassExplicit)
        fileCfg.requirePass = cliCfg.requirePass;
    if (cli.authUsernameExplicit)
        fileCfg.authUsername = cliCfg.authUsername;
    if (cli.metricsEnabledExplicit)
        fileCfg.metricsEnabled = cliCfg.metricsEnabled;
    if (cli.metricsBindAddressExplicit)
        fileCfg.metricsBindAddress = cliCfg.metricsBindAddress;
    if (cli.metricsPortExplicit)
        fileCfg.metricsPort = cliCfg.metricsPort;
    if (cli.tlsEnabledExplicit)
        fileCfg.tlsEnabled = cliCfg.tlsEnabled;
    if (cli.tlsCertPathExplicit)
        fileCfg.tlsCertPath = cliCfg.tlsCertPath;
    if (cli.tlsKeyPathExplicit)
        fileCfg.tlsKeyPath = cliCfg.tlsKeyPath;
    if (cli.notifyKeyspaceEventsExplicit)
        fileCfg.notifyKeyspaceEvents = cliCfg.notifyKeyspaceEvents;
    if (cli.lruRecencyExplicit)
        fileCfg.lruRecency = cliCfg.lruRecency;
    if (cli.cpuAffinityExplicit)
        fileCfg.cpuAffinity = cliCfg.cpuAffinity;
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
                .context = std::format(
                    "{} cannot be combined with --listen / --listen-tls / YAML listeners:; "
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
