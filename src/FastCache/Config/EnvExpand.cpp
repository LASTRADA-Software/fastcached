// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/EnvExpand.hpp>

#include <cstdlib>
#include <optional>
#include <string>

namespace FastCache
{

namespace
{

    /// Look up an environment variable.
    ///
    /// @return The value, empty when the variable is set but empty, or nullopt
    ///         when it is not set at all. The distinction is what lets an unset
    ///         variable be reported rather than silently expanded away.
    [[nodiscard]] std::optional<std::string> LookupEnvironment(std::string const& name)
    {
#if defined(_WIN32)
        // The secure CRT form, so the build stays warning-clean under /WX.
        // getenv_s reports the length INCLUDING the NUL: 0 means not present.
        std::size_t size = 0;
        if (::getenv_s(&size, nullptr, 0, name.c_str()) != 0 || size == 0)
            return std::nullopt;

        std::string value(size, '\0');
        if (::getenv_s(&size, value.data(), size, name.c_str()) != 0)
            return std::nullopt;
        value.resize(size - 1);
        return value;
#else
        char const* const value = std::getenv(name.c_str());
        if (value == nullptr)
            return std::nullopt;
        return std::string { value };
#endif
    }

    [[nodiscard]] constexpr bool IsNameStart(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    }

    [[nodiscard]] constexpr bool IsNameChar(char c) noexcept
    {
        return IsNameStart(c) || (c >= '0' && c <= '9');
    }

    [[nodiscard]] ConfigError MakeError(ConfigErrorCode code, std::string_view field, std::string context)
    {
        return ConfigError {
            .code = code,
            .source = {},
            .line = 0,
            .field = std::string { field },
            .context = std::move(context),
        };
    }

} // namespace

std::expected<std::string, ConfigError> ExpandEnvironmentVariables(std::string_view input, std::string_view field)
{
    std::string out;
    out.reserve(input.size());

    for (std::size_t i = 0; i < input.size();)
    {
        if (input[i] != '$')
        {
            out.push_back(input[i]);
            ++i;
            continue;
        }

        if (i + 1 >= input.size())
            return std::unexpected(MakeError(ConfigErrorCode::ParseError, field, "trailing '$'; write '$$' for a literal"));

        char const next = input[i + 1];

        if (next == '$')
        {
            out.push_back('$');
            i += 2;
            continue;
        }

        std::string name;
        if (next == '{')
        {
            auto const close = input.find('}', i + 2);
            if (close == std::string_view::npos)
                return std::unexpected(MakeError(ConfigErrorCode::ParseError, field, "unterminated '${'"));
            name = std::string { input.substr(i + 2, close - (i + 2)) };
            if (name.empty())
                return std::unexpected(MakeError(ConfigErrorCode::ParseError, field, "empty '${}'"));
            i = close + 1;
        }
        else if (IsNameStart(next))
        {
            auto end = i + 1;
            while (end < input.size() && IsNameChar(input[end]))
                ++end;
            name = std::string { input.substr(i + 1, end - (i + 1)) };
            i = end;
        }
        else
        {
            return std::unexpected(MakeError(ConfigErrorCode::ParseError,
                                             field,
                                             std::string { "'$' followed by '" } + next + "'; write '$$' for a literal"));
        }

        auto value = LookupEnvironment(name);
        if (!value)
            return std::unexpected(
                MakeError(ConfigErrorCode::UndefinedVariable, field, "environment variable is not set: " + name));

        out += *value;
    }

    return out;
}

} // namespace FastCache
