// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Options.hpp>

#include <format>
#include <utility>

namespace FastCache
{

ConfigError ArgvError(ConfigErrorCode code, std::string field, std::string context)
{
    return ConfigError {
        .code = code, .source = "argv", .line = 0, .field = std::move(field), .context = std::move(context)
    };
}

std::string RenderFlagForms(std::string_view primary, std::string_view alias, std::string_view operand)
{
    std::string forms { primary };
    if (!alias.empty())
        forms += std::format(", {}", alias);
    forms += operand;
    return forms;
}

std::expected<std::string_view, ConfigError> TakeValue(std::span<char const* const> args,
                                                       std::size_t& i,
                                                       std::string_view flag)
{
    auto const arg = std::string_view { args[i] };
    if (auto const eq = arg.find('='); eq != std::string_view::npos)
        return arg.substr(eq + 1);
    if (i + 1 >= args.size())
        return std::unexpected(ArgvError(ConfigErrorCode::ParseError, std::string { flag }, "missing value"));
    ++i;
    return std::string_view { args[i] };
}

std::expected<std::string, ConfigError> ParseText(std::string_view sv)
{
    return std::string { sv };
}

} // namespace FastCache
