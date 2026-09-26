// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Config/YamlReader.hpp>

#include <yaml-cpp/yaml.h>

#include <expected>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

namespace FastCache
{

namespace
{

    [[nodiscard]] ConfigError MakeError(
        ConfigErrorCode code, std::filesystem::path const& path, std::string field, std::string context, unsigned line = 0)
    {
        return ConfigError {
            .code = code,
            .source = path.string(),
            .line = line,
            .field = std::move(field),
            .context = std::move(context),
        };
    }

    [[nodiscard]] std::expected<YAML::Node, ConfigError> LoadRoot(std::filesystem::path const& path)
    {
        try
        {
            return YAML::LoadFile(path.string());
        }
        catch (YAML::BadFile const&)
        {
            // Caught BEFORE the generic handler, which is what makes the code
            // right: `YAML::BadFile` derives from `YAML::Exception`, so a path that
            // does not exist used to arrive as `ParseError` carrying yaml-cpp's
            // "bad file: <path>". A missing file is the commonest way to get this
            // wrong -- a typo in `--config` -- and `ParseError` sends an operator
            // to look for a syntax mistake in a file that is not there.
            //
            // The code is what monitoring and every caller switch on, and
            // `FileNotFound` is documented for exactly this. yaml-cpp cannot tell
            // absent from unreadable, so the sentence says both rather than
            // asserting the one it did not check.
            return std::unexpected(
                MakeError(ConfigErrorCode::FileNotFound, path, {}, "no such file, or this account may not read it"));
        }
        catch (YAML::ParserException const& e)
        {
            return std::unexpected(
                MakeError(ConfigErrorCode::ParseError, path, {}, e.msg, static_cast<unsigned>(e.mark.line + 1)));
        }
        catch (YAML::Exception const& e)
        {
            return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, e.what()));
        }
    }

    /// One-based YAML source line for an error message. yaml-cpp's `Mark::line`
    /// is zero-based and signed; this normalises to the human convention.
    /// @param node Source node whose mark is consulted.
    /// @return One-based line number, or 0 if the mark is missing.
    [[nodiscard]] unsigned YamlLine(YAML::Node const& node) noexcept
    {
        auto const raw = node.Mark().line;
        return raw < 0 ? 0U : static_cast<unsigned>(raw) + 1U;
    }

} // namespace

std::expected<std::vector<YamlSetting>, ConfigError> ReadYamlSettings(std::filesystem::path const& path)
{
    auto root = LoadRoot(path);
    if (!root.has_value())
        return std::unexpected(root.error());

    // A null document is an empty file, or one that is entirely comments. Both are
    // ordinary: both reference configurations this project ships are nothing BUT
    // commented-out keys, so an operator who has not uncommented any of them yet has
    // a valid configuration rather than a broken one -- which is every fresh package
    // install.
    if (root->IsNull())
        return std::vector<YamlSetting> {};
    if (!root->IsMap())
        return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "top-level must be a map"));

    std::vector<YamlSetting> settings;
    for (auto const& kv: *root)
    {
        // Asked before the key is read as text, because reading a map or a sequence
        // as text THROWS -- and nothing above this catches it, so `? [a, b]` ended a
        // worker with an unhandled exception rather than a refusal naming the line.
        if (!kv.first.IsScalar())
            return std::unexpected(MakeError(ConfigErrorCode::ParseError, path, {}, "non-scalar key", YamlLine(kv.first)));

        auto const key = kv.first.as<std::string>();
        auto const& value = kv.second;
        YamlSetting setting { .key = key, .values = {}, .line = YamlLine(kv.first) };

        // A key written twice is refused by name. The document is still a map to the
        // parser, which hands over BOTH entries, so each caller would otherwise decide
        // what a repetition means by accident: a scalar row keeps the LAST value and a
        // list row APPENDS both -- two silent answers, neither of them anything the
        // operator can see, and the first of them discards a value somebody wrote.
        if (auto const* const earlier = core::findOrNull(settings, key, &YamlSetting::key); earlier != nullptr)
            return std::unexpected(MakeError(ConfigErrorCode::ParseError,
                                             path,
                                             key,
                                             std::format("named twice (first at line {}); a repeated key would "
                                                         "silently keep one of the values written",
                                                         earlier->line),
                                             setting.line));

        if (value.IsScalar())
            setting.values.push_back(value.as<std::string>());
        else if (value.IsSequence())
        {
            for (auto const& element: value)
            {
                // Refused rather than flattened or skipped. A caller applies these
                // through a table of appliers that each take one string, so there is
                // no representation for a nested element -- and dropping it silently
                // would be a setting an operator wrote and nothing read, which is the
                // failure this whole mechanism exists to remove.
                if (!element.IsScalar())
                    return std::unexpected(MakeError(
                        ConfigErrorCode::TypeMismatch, path, key, "sequence elements must be scalars", YamlLine(element)));
                setting.values.push_back(element.as<std::string>());
            }
        }
        else if (!value.IsNull())
            return std::unexpected(MakeError(
                ConfigErrorCode::TypeMismatch, path, key, "expected a scalar or a sequence of scalars", setting.line));

        // A key present with no value (`toolchain:` and nothing after it) carries no
        // values and is kept rather than dropped: the caller decides whether naming a
        // setting and giving it nothing is meaningful, and for a list-valued row it
        // legitimately means "empty this".
        settings.push_back(std::move(setting));
    }
    return settings;
}

} // namespace FastCache
