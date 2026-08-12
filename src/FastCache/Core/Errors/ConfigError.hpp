// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace FastCache
{

/// Categories of configuration errors (CLI parsing, YAML parsing, validation, reload).
enum class ConfigErrorCode : std::uint8_t
{
    Ok = 0,            ///< Sentinel.
    FileNotFound,      ///< A named configuration file does not exist or cannot be read.
    ParseError,        ///< YAML/CLI input is syntactically invalid.
    UnknownKey,        ///< YAML contains a key we do not recognise.
    TypeMismatch,      ///< Field present but wrong type (e.g., string where int expected).
    OutOfRange,        ///< Numeric value outside the valid range (e.g., port > 65535).
    MissingRequired,   ///< Required field absent.
    ImmutableChanged,  ///< Reload attempted to change a field that is fixed at startup.
    UndefinedVariable, ///< Value references an environment variable that is not set.
    WriteFailed,       ///< Configuration could not be written to its destination.
};

[[nodiscard]] constexpr std::string_view ToStringView(ConfigErrorCode code) noexcept
{
    switch (code)
    {
        case ConfigErrorCode::Ok:
            return "Ok";
        case ConfigErrorCode::FileNotFound:
            return "FileNotFound";
        case ConfigErrorCode::ParseError:
            return "ParseError";
        case ConfigErrorCode::UnknownKey:
            return "UnknownKey";
        case ConfigErrorCode::TypeMismatch:
            return "TypeMismatch";
        case ConfigErrorCode::OutOfRange:
            return "OutOfRange";
        case ConfigErrorCode::MissingRequired:
            return "MissingRequired";
        case ConfigErrorCode::ImmutableChanged:
            return "ImmutableChanged";
        case ConfigErrorCode::UndefinedVariable:
            return "UndefinedVariable";
        case ConfigErrorCode::WriteFailed:
            return "WriteFailed";
    }
    return "Unknown";
}

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
        // The name, not the number: an operator reading "code=9" out of a
        // startup failure learns nothing the enum already spells out.
        return std::format(
            "ConfigError(code={} source={}:{} field={} context={})", ToStringView(code), source, line, field, context);
    }
};

} // namespace FastCache
