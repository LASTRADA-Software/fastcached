// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/Errors/ConfigError.hpp>

#include <algorithm>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace FastCache
{

/// Applying a configuration FILE through the same option table the command line
/// uses, so a setting has one applier rather than two that can disagree.
///
/// ## Why this is not a merge
///
/// The daemon reads YAML into a `Config`, parses argv into a `CliResult`, and then
/// merges the two field by field -- which needs a per-field row list, a per-field
/// explicit bit, and a per-field presence bit, none derived from the others. Four
/// lists that must agree, and the file's own comments record that a flag which
/// parses but never merges has been shipped four times.
///
/// Here the file's values and the command line's reach the config through the SAME
/// `apply`, in that order, so "CLI wins" is the order they are applied in and there
/// is nothing to keep in step. A row that parses cannot fail to merge, because
/// parsing and merging stopped being two things.
///
/// ## It lives in `Config/`, not `Cli/`
///
/// `Cli/Options.hpp` is compiled into `fastcache-cc`, which does not link
/// `FastCache` -- so an include of anything from `Config/` there breaks the
/// launcher's link rather than merely its build. This header is where the option
/// table meets the YAML reader, which is why it is on this side of that line.

/// Apply file settings to `result` through `table`.
///
/// Each setting is matched to the row whose `yamlKey` it names, and every value it
/// carried is passed to that row's `apply` -- once for a scalar, once per element
/// for a sequence. The row's `explicitBit` is then set, because a key in a file is
/// the operator naming the setting; which instance of the result that bit is read
/// from is what distinguishes "named it anywhere" from "typed it on the command
/// line" (see the node's `main`).
///
/// A key naming no row is refused rather than ignored. A file is read at every
/// start, so a key nothing reads is a setting an operator believes is in force
/// forever -- the exact failure this mechanism exists to remove, and a typo is the
/// common way to reach it.
///
/// **Errors are attributed to the FILE, and name the key rather than the flag.**
/// `ApplyOneOption` stamps a row's `primary` for the same reason -- a shared
/// value parser cannot know which row reached it -- but stamping `--cache-memory`
/// on a bad `cache_memory:` would send an operator to look at a command line they
/// never typed. Same mechanism, deliberately different value.
/// @param table The rows to match against.
/// @param settings What the file carried, in document order.
/// @param path The file, for error attribution.
/// @param result The result to populate.
/// @return Nothing, or the first setting that could not be applied.
template <typename Result>
[[nodiscard]] std::expected<void, ConfigError> ApplyFileSettings(std::span<OptionSpec<Result> const> table,
                                                                 std::vector<YamlSetting> const& settings,
                                                                 std::filesystem::path const& path,
                                                                 Result& result)
{
    auto const fileError = [&path](ConfigErrorCode code, std::string_view key, std::string context, unsigned line) {
        return ConfigError {
            .code = code, .source = path.string(), .line = line, .field = std::string { key }, .context = std::move(context)
        };
    };

    for (auto const& setting: settings)
    {
        auto const row = std::ranges::find_if(table, [&setting](OptionSpec<Result> const& spec) {
            return !spec.yamlKey.empty() && spec.yamlKey == setting.key;
        });
        if (row == std::ranges::end(table))
            return std::unexpected(fileError(ConfigErrorCode::UnknownKey,
                                             setting.key,
                                             "no such setting; a key nothing reads is a setting that never takes effect",
                                             setting.line));

        // A key present with no value empties a list setting -- `toolchain:` with
        // nothing under it is "serve no discovered toolchain", which an operator can
        // legitimately mean. For a scalar there is no such reading, and guessing one
        // (the empty string? the default?) would be a value nobody wrote.
        if (setting.values.empty())
        {
            if (row->clear == nullptr)
                return std::unexpected(
                    fileError(ConfigErrorCode::TypeMismatch, setting.key, "setting names no value", setting.line));
            (void) (*row->clear)(result, {});
        }

        // A sequence under a key that is not a list is refused rather than folded.
        // Applying each in turn would leave the last one winning, which is a value
        // the operator wrote being silently discarded in favour of another value the
        // operator also wrote -- the one case where guessing is least defensible.
        if (row->clear == nullptr && setting.values.size() > 1)
            return std::unexpected(
                fileError(ConfigErrorCode::TypeMismatch, setting.key, "setting takes one value, not a list", setting.line));

        for (auto const& value: setting.values)
        {
            // A flag whose meaning is its PRESENCE is spelled as a boolean here, and
            // `apply` runs on `true` alone -- see `TableIsWellFormed`. Only `true`
            // and `false` are accepted: YAML 1.1's `yes`/`on` are a spelling this
            // reader would have to reproduce exactly to be trusted, and refusing a
            // word beats accepting one whose meaning came from a different schema.
            if (row->arity == Arity::None)
            {
                if (value != "true" && value != "false")
                    return std::unexpected(
                        fileError(ConfigErrorCode::TypeMismatch, setting.key, "expected true or false", setting.line));
                if (value == "false")
                    continue;
            }

            if (auto applied = (*row->apply)(result, value); !applied.has_value())
            {
                auto error = std::move(applied).error();
                error.source = path.string();
                error.line = setting.line;
                error.field = std::string { setting.key };
                return std::unexpected(std::move(error));
            }
        }

        if (row->explicitBit != nullptr)
            result.*row->explicitBit = true;
    }
    return {};
}

/// Empty every list setting the command line names, before the command line is
/// applied over a file-seeded result.
///
/// A repeatable row's applier APPENDS, so without this a `--toolchain` on the
/// command line would extend the file's list rather than replace it. Replacement is
/// the daemon's rule for `listeners:` and its reasoning is what settles it: mixing
/// partial file values with partial command-line values makes precedence depend on
/// declaration order, which is not something an operator can reason about.
///
/// Driven off the table's `clear` column rather than a hand-written list of list
/// settings, so a fourth repeatable flag is one column value rather than an edit
/// here that somebody has to remember.
/// @param table The rows to match against.
/// @param args The command line, program name already removed.
/// @param result The file-seeded result to reset in place.
template <typename Result>
void ClearListsNamedOn(std::span<OptionSpec<Result> const> table, std::span<char const* const> args, Result& result)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string_view const arg { args[i] };
        auto const row = std::ranges::find_if(table, [arg](OptionSpec<Result> const& spec) { return Matches(arg, spec); });

        // A token naming no row is left alone rather than reported: the caller has
        // already parsed this argv once and refused it there, with the message an
        // operator wants. A second refusal here would only change which sentence
        // they see.
        if (row == std::ranges::end(table))
            continue;

        // A VALUE is not a flag, and only the parser's own rule can tell them
        // apart. `--advertise --toolchain` gives `--advertise` the value
        // "--toolchain"; a scan of every token for a spelling would read that value
        // as naming the list and silently empty what the file declared. So the value
        // is consumed here exactly as `ApplyOneOption` consumes it -- through
        // `TakeValue`, which advances past a separate-argument value and leaves an
        // attached one alone. Its outcome is discarded because a missing value was
        // already refused by that same earlier parse.
        if (row->arity == Arity::Value)
            (void) TakeValue(args, i, row->primary);

        if (row->clear != nullptr)
            (void) (*row->clear)(result, {});
    }
}

} // namespace FastCache
