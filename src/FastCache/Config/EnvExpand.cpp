// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/EnvExpand.hpp>
#include <FastCache/Platform/Environment.hpp>

#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace FastCache
{

namespace
{

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

        // A view into `input`: ReadEnvironmentVariable owns the one
        // NUL-terminating copy the platform APIs need.
        std::string_view name;
        if (next == '{')
        {
            auto const close = input.find('}', i + 2);
            if (close == std::string_view::npos)
                return std::unexpected(MakeError(ConfigErrorCode::ParseError, field, "unterminated '${'"));
            name = input.substr(i + 2, close - (i + 2));
            if (name.empty())
                return std::unexpected(MakeError(ConfigErrorCode::ParseError, field, "empty '${}'"));
            i = close + 1;
        }
        else if (IsNameStart(next))
        {
            auto end = i + 1;
            while (end < input.size() && IsNameChar(input[end]))
                ++end;
            name = input.substr(i + 1, end - (i + 1));
            i = end;
        }
        else
        {
            return std::unexpected(MakeError(ConfigErrorCode::ParseError,
                                             field,
                                             std::string { "'$' followed by '" } + next + "'; write '$$' for a literal"));
        }

        auto value = ReadEnvironmentVariable(name);
        if (!value)
            return std::unexpected(MakeError(
                ConfigErrorCode::UndefinedVariable, field, std::format("environment variable is not set: {}", name)));

        out += *value;
    }

    return out;
}

} // namespace FastCache
