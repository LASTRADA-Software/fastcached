// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Config/Config.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace FastCache
{

/// Parse a YAML file into a Config. Unknown keys yield ConfigError(UnknownKey);
/// wrong types yield ConfigError(TypeMismatch); out-of-range numbers yield
/// ConfigError(OutOfRange). Missing file is ConfigError(FileNotFound).
///
/// Recognised top-level keys (all optional):
///
///   bind:        string   interface to bind on (e.g. "127.0.0.1", "0.0.0.0").
///
///   port:        int      TCP listen port; 1..65535.
///
///   max_memory:  size     in-memory cache budget. Integer with optional unit
///                         suffix: k/K = 1024, m/M = 1024², g/G = 1024³
///                         (1024-based). Plain integer means bytes. A trailing
///                         "%" sets the budget to that percentage of host total
///                         RAM (e.g., 50%). 0 disables eviction.
///                         Examples: 67108864, 64m, 1g, 50%.
///
///   log_level:   string   one of: trace | debug | info | warn | error | fatal.
///
/// CLI flags later override these values via the caller.
/// @param path Filesystem path of the YAML file.
/// @return Parsed Config or a ConfigError.
[[nodiscard]] std::expected<Config, ConfigError> ReadYamlConfig(std::filesystem::path const& path);

/// Parsed YAML, plus per-field "this key was present in the file" bits. The
/// presence bits let the caller distinguish "the YAML explicitly set this to
/// the default value" from "the YAML did not mention it" — important for the
/// env-precedence guards in main.cpp, which would otherwise silently override
/// an operator's explicit YAML value when it happens to equal the compiled
/// default.
struct YamlConfigWithPresence
{
    Config config {};
    bool metricsPortExplicit { false };
    bool metricsBindAddressExplicit { false };
    bool metricsEnabledExplicit { false };
    bool requirePassExplicit { false };
    bool authUsernameExplicit { false };
    bool tlsEnabledExplicit { false };
    bool tlsCertPathExplicit { false };
    bool tlsKeyPathExplicit { false };
    /// Whether the YAML carried a `bind:` key. Together with
    /// `portExplicit` this lets main.cpp reject a config that mixes the
    /// legacy single-bind shape with `listeners:` — they would otherwise
    /// silently pick `listeners:` and discard `bind:`/`port:`.
    bool bindAddressExplicit { false };
    bool portExplicit { false };
};

/// Variant of `ReadYamlConfig` that also reports which keys were explicitly
/// present in the file. Used by main.cpp's env-precedence logic.
/// @param path Filesystem path of the YAML file.
/// @return Parsed config + presence bits, or a ConfigError.
[[nodiscard]] std::expected<YamlConfigWithPresence, ConfigError> ReadYamlConfigWithPresence(
    std::filesystem::path const& path);

/// One top-level key from a YAML document, with the scalar values it carried.
struct YamlSetting
{
    /// The top-level key, verbatim. Never interpreted here.
    std::string key;

    /// One entry for a scalar; one per element for a sequence of scalars.
    ///
    /// A sequence rather than a second shape, because that is what a repeatable
    /// setting looks like in YAML and a caller applying values one at a time does
    /// not need to know which spelling it met.
    std::vector<std::string> values;

    /// One-based source line of the key, or 0 when the document would not say.
    /// Carried so a rejection points at the line an operator has to edit.
    unsigned line { 0 };
};

/// Read a YAML file as top-level key/scalar settings, without knowing what any
/// key MEANS.
///
/// The generic door `ReadYamlConfig` never had: that function parses straight into
/// the daemon's `Config` through a hand-written key ladder, so a second binary had
/// no way in. This returns the file's shape and nothing else, which is what lets a
/// caller drive its own option table from it.
///
/// **yaml-cpp stays out of the header** — values come back as `std::string`, so the
/// dependency remains an implementation detail of this translation unit exactly as
/// it is for `ReadYamlConfig`.
///
/// Deliberately shallow. A value that is a map, or a sequence containing anything
/// but scalars, is a `TypeMismatch` naming the key rather than something flattened:
/// a caller that applies values through a table has no way to represent nesting,
/// and silently ignoring a nested block would be a setting an operator wrote and
/// nothing read. The daemon's own `listeners:` is nested, which is why that reader
/// stays where it is rather than being rebuilt on this.
///
/// An empty document is success carrying nothing, because a fully-commented
/// reference file is a legitimate and expected configuration.
/// @param path Filesystem path of the YAML file.
/// @return The settings in document order, or why the file could not be read.
[[nodiscard]] std::expected<std::vector<YamlSetting>, ConfigError> ReadYamlSettings(std::filesystem::path const& path);

} // namespace FastCache
