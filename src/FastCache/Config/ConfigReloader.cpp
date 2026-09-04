// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/CliParser.hpp>
#include <FastCache/Config/ConfigMerge.hpp>
#include <FastCache/Config/ConfigReloader.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache
{

ConfigReloader::ConfigReloader(Config initial, std::filesystem::path configPath, ConfigSources sources):
    ConfigReloaderOf<Config> { std::move(initial),
                               std::move(configPath),
                               // The SAME assembly the start ran, with the same sources -- not
                               // `ReadYamlConfig(path)`, which is the file and nothing else. A reload built
                               // that way republished every setting the file did not mention at its
                               // built-in default, so `--max-memory=8g` came back as a fraction of host RAM
                               // and the storage evicted down to it, with no line anywhere naming the flag
                               // (#622). Captured by value because the reloader outlives `main`'s locals
                               // and a reload can run at any moment on the signal thread.
                               [sources = std::move(sources)](
                                   std::filesystem::path const& path) -> std::expected<Config, ConfigError> {
                                   // By value rather than by rvalue reference: the file's
                                   // presence bits are a START's question, so only the
                                   // configuration is carried on, and a parameter this
                                   // moves a MEMBER out of is one nothing moves from.
                                   return AssembleEffectiveConfig(path, sources).transform([](EffectiveConfig assembled) {
                                       return std::move(assembled.config);
                                   });
                               },
                               &ConfigReloader::ValidateImmutable }
{
}

ConfigError ImmutableChangedError(std::span<std::string_view const> changed)
{
    std::string names;
    for (auto const& name: changed)
    {
        if (!names.empty())
            names += ", ";
        names += name;
    }

    return ConfigError {
        .code = ConfigErrorCode::ImmutableChanged,
        .source = {},
        .line = 0,
        // The FIRST one, because the field is one string and something has to go in
        // it; the whole list is in the context, which is what an operator reads.
        .field = changed.empty() ? std::string {} : std::string { changed.front() },
        .context = std::format("not reloadable, so nothing was applied: {}", names),
    };
}

std::vector<std::string_view> UnreloadableChanges(Config const& previous, Config const& candidate)
{
    std::vector<std::string_view> changed;
    for (auto const& setting: ConfigFileSettings())
        // `!= Yes`, never `== No`: `Reloadable`'s own header argues that a forgotten
        // column must fail CLOSED, and a predicate spelled `== No` inverts that the
        // day a third enumerator appears -- the new state would be silently
        // reloadable. `NodeConfig.cpp`'s `UnreloadableChanges` spells it this way for
        // the same reason, and one rule read two ways is how the two tables drift.
        if (setting.reloadable != Reloadable::Yes && !setting.same(previous, candidate))
            changed.push_back(setting.key);
    return changed;
}

std::expected<void, ConfigError> ConfigReloader::ValidateImmutable(Config const& previous, Config const& candidate)
{
    auto const changed = UnreloadableChanges(previous, candidate);
    if (changed.empty())
        return {};
    // The YAML KEY, not the flag: the operator edited a file and typed
    // `storage_path`, so naming `--storage` at them answers a question they did not
    // ask. `ConfigFileSettings()` carries keys for exactly that reason.
    return std::unexpected(ImmutableChangedError(changed));
}

} // namespace FastCache
