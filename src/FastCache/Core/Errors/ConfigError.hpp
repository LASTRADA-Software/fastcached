// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of configuration errors (CLI parsing, YAML parsing, validation, reload).
enum class ConfigErrorCode : unsigned
{
    Ok = 0,             ///< Sentinel.
    FileNotFound,       ///< --config path does not exist or cannot be read.
    ParseError,         ///< YAML/CLI input is syntactically invalid.
    UnknownKey,         ///< YAML contains a key we do not recognise.
    TypeMismatch,       ///< Field present but wrong type (e.g., string where int expected).
    OutOfRange,         ///< Numeric value outside the valid range (e.g., port > 65535).
    MissingRequired,    ///< Required field absent.
    ImmutableChanged,   ///< Reload attempted to change a field that is fixed at startup.
};

/// Structured config error. Carries file:line if the source supports it.
struct ConfigError
{
    ConfigErrorCode code = ConfigErrorCode::ParseError;

    /// Source descriptor: file path, "argv", or empty when not applicable.
    std::string source;

    /// 1-based line number in the source, or 0 if unknown.
    unsigned line = 0;

    /// Field/key the error refers to, if known.
    std::string field;

    /// Free-form context.
    std::string context;

    [[nodiscard]] std::string ToString() const
    {
        return std::format("ConfigError(code={} source={}:{} field={} context={})",
                           static_cast<unsigned>(code),
                           source,
                           line,
                           field,
                           context);
    }
};

[[nodiscard]] constexpr std::string_view ToStringView(ConfigErrorCode code) noexcept
{
    switch (code)
    {
        case ConfigErrorCode::Ok:               return "Ok";
        case ConfigErrorCode::FileNotFound:     return "FileNotFound";
        case ConfigErrorCode::ParseError:       return "ParseError";
        case ConfigErrorCode::UnknownKey:       return "UnknownKey";
        case ConfigErrorCode::TypeMismatch:     return "TypeMismatch";
        case ConfigErrorCode::OutOfRange:       return "OutOfRange";
        case ConfigErrorCode::MissingRequired:  return "MissingRequired";
        case ConfigErrorCode::ImmutableChanged: return "ImmutableChanged";
    }
    return "Unknown";
}

} // namespace FastCache
