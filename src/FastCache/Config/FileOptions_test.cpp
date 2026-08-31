// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Config/FileOptions.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <span>
#include <string>
#include <vector>

using namespace FastCache;

namespace
{
/// A stand-in result with one row of every shape the file layer distinguishes:
/// a scalar, a repeatable list, a flag whose meaning is its presence, and a
/// scalar carrying an explicit bit.
///
/// Deliberately not `NodeConfig`. What is under test here is the mechanism --
/// which rows a key may reach, what a boolean means, what a list does when the
/// command line also names one -- and asserting it against the worker's real
/// 36-row table would tie every case to a flag somebody may rename.
struct Sample
{
    std::string name;
    std::vector<std::string> items;
    bool loud { false };
    bool quiet { true };
    std::string tuned;
    bool nameExplicit { false };
    bool tunedExplicit { false };
};

[[nodiscard]] std::expected<std::string, ConfigError> ParseAnything(std::string_view value)
{
    return std::string { value };
}

/// Refuses everything, so a row can be driven into its applier's error path --
/// which is where the file layer has to restamp the source, line and key.
[[nodiscard]] std::expected<std::string, ConfigError> ParseNothing(std::string_view /*value*/)
{
    return std::unexpected(ConfigError { .code = ConfigErrorCode::OutOfRange,
                                         .source = "argv",
                                         .line = 0,
                                         .field = "--tuned",
                                         .context = "never acceptable" });
}

[[nodiscard]] std::span<OptionSpec<Sample> const> SampleOptions() noexcept
{
    static constexpr auto options = std::to_array<OptionSpec<Sample>>({
        { .primary = "--name",
          .arity = Arity::Value,
          .operand = "=<text>",
          .apply = AssignFrom<&Sample::name, ParseAnything>(),
          .explicitBit = &Sample::nameExplicit,
          .description = "a scalar setting",
          .yamlKey = "name" },
        { .primary = "--item",
          .arity = Arity::Value,
          .operand = "=<text>",
          .apply = AppendFrom<&Sample::items, ParseAnything>(),
          .description = "a repeatable setting",
          .yamlKey = "item",
          .clear = ClearList<&Sample::items>() },
        { .primary = "--loud",
          .arity = Arity::None,
          .apply = SetTrue<&Sample::loud>(),
          .description = "a flag whose meaning is its presence",
          .yamlKey = "loud" },
        { .primary = "--no-quiet",
          .arity = Arity::None,
          .apply = SetFalse<&Sample::quiet>(),
          .description = "the same, negatively named",
          .yamlKey = "no_quiet" },
        { .primary = "--tuned",
          .arity = Arity::Value,
          .operand = "=<text>",
          .apply = AssignFrom<&Sample::tuned, ParseNothing>(),
          .explicitBit = &Sample::tunedExplicit,
          .description = "a setting whose parser always refuses",
          .yamlKey = "tuned" },
        { .primary = "--now", .arity = Arity::None, .apply = SetTrue<&Sample::loud>(), .description = "no key" },
    });
    static_assert(TableIsWellFormed<Sample>(options));
    return options;
}

[[nodiscard]] YamlSetting Scalar(std::string key, std::string value, unsigned line = 1)
{
    return YamlSetting { .key = std::move(key), .values = { std::move(value) }, .line = line };
}
} // namespace

TEST_CASE("FileOptions: a setting in the file takes effect", "[config][file]")
{
    Sample result;
    auto const applied =
        ApplyFileSettings(SampleOptions(), { Scalar("name", "from-the-file") }, std::filesystem::path { "c.yaml" }, result);

    REQUIRE(applied.has_value());
    CHECK(result.name == "from-the-file");
}

TEST_CASE("FileOptions: the command line wins over the file", "[config][file]")
{
    // Not a merge and not a precedence table: both reach the field through the
    // same applier, and "the command line wins" is which of the two runs second.
    Sample result;
    REQUIRE(ApplyFileSettings(SampleOptions(), { Scalar("name", "file") }, std::filesystem::path { "c.yaml" }, result)
                .has_value());

    auto const args = std::to_array<char const*>({ "--name=argv" });
    REQUIRE(ParseOptionsInto(SampleOptions(), std::span<char const* const> { args }, result).has_value());
    CHECK(result.name == "argv");
}

TEST_CASE("FileOptions: a key naming no row is refused, not ignored", "[config][file]")
{
    // The failure this whole mechanism exists to remove. A file is read at every
    // start, so a key nothing reads is a setting an operator believes is in force
    // forever, and a typo is the ordinary way to get one.
    Sample result;
    auto const applied =
        ApplyFileSettings(SampleOptions(), { Scalar("nmae", "typo", 7) }, std::filesystem::path { "c.yaml" }, result);

    REQUIRE(!applied.has_value());
    CHECK(applied.error().code == ConfigErrorCode::UnknownKey);
    CHECK(applied.error().field == "nmae");
    CHECK(applied.error().line == 7);
    CHECK(applied.error().source == "c.yaml");
}

TEST_CASE("FileOptions: a row carrying no key is unreachable from a file", "[config][file]")
{
    // `--now` is a row with no `yamlKey`. Reaching it by its flag spelling, or by
    // the name a convention would derive, must both be unknown keys rather than a
    // second door into a setting the table says a file may not carry.
    Sample result;
    for (auto const& key: { "--now", "now" })
    {
        auto const applied =
            ApplyFileSettings(SampleOptions(), { Scalar(key, "true") }, std::filesystem::path { "c.yaml" }, result);
        INFO("key: " << key);
        REQUIRE(!applied.has_value());
        CHECK(applied.error().code == ConfigErrorCode::UnknownKey);
    }
    CHECK(!result.loud);
}

TEST_CASE("FileOptions: a presence flag is a boolean, and only true applies it", "[config][file]")
{
    // Both polarities, because the reading is only correct if it holds for both:
    // the key spells the FLAG, so `no_quiet: false` is "do not pass --no-quiet",
    // which leaves `quiet` at its default rather than setting it.
    SECTION("true applies the flag")
    {
        Sample result;
        REQUIRE(ApplyFileSettings(SampleOptions(),
                                  { Scalar("loud", "true"), Scalar("no_quiet", "true") },
                                  std::filesystem::path { "c.yaml" },
                                  result)
                    .has_value());
        CHECK(result.loud);
        CHECK(!result.quiet);
    }

    SECTION("false does nothing at all")
    {
        Sample result;
        REQUIRE(ApplyFileSettings(SampleOptions(),
                                  { Scalar("loud", "false"), Scalar("no_quiet", "false") },
                                  std::filesystem::path { "c.yaml" },
                                  result)
                    .has_value());
        CHECK(!result.loud);
        CHECK(result.quiet);
    }

    SECTION("anything else is refused rather than guessed")
    {
        // YAML 1.1's `yes`/`on` are a spelling this reader would have to reproduce
        // exactly to be trusted. Accepting `yes` and rejecting `y` -- or the other
        // way round -- is a boolean whose meaning came from a schema nobody chose.
        Sample result;
        for (auto const& value: { "yes", "on", "1", "True", "" })
        {
            auto const applied =
                ApplyFileSettings(SampleOptions(), { Scalar("loud", value) }, std::filesystem::path { "c.yaml" }, result);
            INFO("value: " << value);
            REQUIRE(!applied.has_value());
            CHECK(applied.error().code == ConfigErrorCode::TypeMismatch);
            CHECK(applied.error().field == "loud");
        }
        CHECK(!result.loud);
    }
}

TEST_CASE("FileOptions: a key with no value empties a list and refuses a scalar", "[config][file]")
{
    Sample result;
    result.items = { "a", "b" };

    SECTION("a list row is emptied")
    {
        // `item:` with nothing under it is "serve none", which an operator can
        // legitimately mean and which no other spelling expresses.
        auto const applied = ApplyFileSettings(SampleOptions(),
                                               { YamlSetting { .key = "item", .values = {}, .line = 3 } },
                                               std::filesystem::path { "c.yaml" },
                                               result);
        REQUIRE(applied.has_value());
        CHECK(result.items.empty());
    }

    SECTION("a scalar row has no such reading")
    {
        // The empty string? The default? Both are values nobody wrote.
        auto const applied = ApplyFileSettings(SampleOptions(),
                                               { YamlSetting { .key = "name", .values = {}, .line = 3 } },
                                               std::filesystem::path { "c.yaml" },
                                               result);
        REQUIRE(!applied.has_value());
        CHECK(applied.error().code == ConfigErrorCode::TypeMismatch);
        CHECK(applied.error().line == 3);
    }
}

TEST_CASE("FileOptions: a sequence under a scalar key is refused", "[config][file]")
{
    // Applying each in turn would leave the last winning, which is one value the
    // operator wrote silently discarded in favour of another value they also wrote.
    Sample result;
    auto const applied = ApplyFileSettings(SampleOptions(),
                                           { YamlSetting { .key = "name", .values = { "one", "two" }, .line = 4 } },
                                           std::filesystem::path { "c.yaml" },
                                           result);
    REQUIRE(!applied.has_value());
    CHECK(applied.error().code == ConfigErrorCode::TypeMismatch);
    CHECK(applied.error().field == "name");
}

TEST_CASE("FileOptions: an applier's own refusal is re-attributed to the file", "[config][file]")
{
    // A shared value parser cannot know which row reached it, so it names the flag.
    // Left alone, a bad `tuned:` would send an operator to look at a `--tuned` on a
    // command line they never typed.
    Sample result;
    auto const applied =
        ApplyFileSettings(SampleOptions(), { Scalar("tuned", "nope", 11) }, std::filesystem::path { "/etc/c.yaml" }, result);

    REQUIRE(!applied.has_value());
    CHECK(applied.error().code == ConfigErrorCode::OutOfRange);
    CHECK(applied.error().source == "/etc/c.yaml");
    CHECK(applied.error().line == 11);
    CHECK(applied.error().field == "tuned");
}

TEST_CASE("FileOptions: naming a setting in a file sets its explicit bit", "[config][file]")
{
    // Provenance is "did the operator name this", and a file is one of the two
    // places they can. The bit is what a startup decision reads -- `--cache-memory`
    // pinned to its own default is a different instruction from one nobody typed --
    // so a setting that arrived from the file has to carry it.
    Sample result;
    REQUIRE(!result.nameExplicit);
    REQUIRE(
        ApplyFileSettings(SampleOptions(), { Scalar("name", "x") }, std::filesystem::path { "c.yaml" }, result).has_value());
    CHECK(result.nameExplicit);

    // And only for the row the key named: a bit set for every row would make the
    // whole struct look typed, which is the same as having no provenance at all.
    CHECK(!result.tunedExplicit);
}

TEST_CASE("FileOptions: the file's provenance is a DIFFERENT result from the command line's", "[config][file]")
{
    // The two questions are "named it anywhere" and "typed it on the command line",
    // and they are answered by reading the bit off two different objects. A service
    // registration asks the second -- it replays a command line, so a value the FILE
    // supplied must not be baked into it -- and reads it from the argv-only parse.
    Sample fromFile;
    REQUIRE(ApplyFileSettings(SampleOptions(), { Scalar("name", "file") }, std::filesystem::path { "c.yaml" }, fromFile)
                .has_value());
    CHECK(fromFile.nameExplicit);

    Sample fromArgv;
    auto const args = std::to_array<char const*>({ "--item=x" });
    REQUIRE(ParseOptionsInto(SampleOptions(), std::span<char const* const> { args }, fromArgv).has_value());
    CHECK(!fromArgv.nameExplicit);
}

TEST_CASE("FileOptions: a command line naming a list replaces the file's", "[config][file]")
{
    // A repeatable row APPENDS, so without the reset the command line would extend
    // the file's list. Replacement is the rule because mixing partial file values
    // with partial command-line values makes precedence depend on declaration
    // order, which is not something an operator can reason about.
    Sample result;
    REQUIRE(ApplyFileSettings(SampleOptions(),
                              { YamlSetting { .key = "item", .values = { "file-a", "file-b" }, .line = 1 } },
                              std::filesystem::path { "c.yaml" },
                              result)
                .has_value());
    REQUIRE(result.items.size() == 2);

    auto const args = std::to_array<char const*>({ "--item=argv-a" });
    std::span<char const* const> const argv { args };
    ClearListsNamedOn(SampleOptions(), argv, result);
    REQUIRE(ParseOptionsInto(SampleOptions(), argv, result).has_value());

    CHECK(result.items == std::vector<std::string> { "argv-a" });
}

TEST_CASE("FileOptions: a command line naming no list leaves the file's alone", "[config][file]")
{
    // The other half, and the one a reset written as "always clear" would break:
    // an operator who passes an unrelated flag has not asked to forget the file's
    // toolchains.
    Sample result;
    REQUIRE(ApplyFileSettings(SampleOptions(),
                              { YamlSetting { .key = "item", .values = { "file-a" }, .line = 1 } },
                              std::filesystem::path { "c.yaml" },
                              result)
                .has_value());

    auto const args = std::to_array<char const*>({ "--name=argv" });
    std::span<char const* const> const argv { args };
    ClearListsNamedOn(SampleOptions(), argv, result);
    REQUIRE(ParseOptionsInto(SampleOptions(), argv, result).has_value());

    CHECK(result.items == std::vector<std::string> { "file-a" });
    CHECK(result.name == "argv");
}

TEST_CASE("FileOptions: a list flag with an attached value still clears", "[config][file]")
{
    // `--item=x` and `--item x` are the same instruction, and the reset walks argv
    // itself rather than the parse -- so it has to recognise both spellings or one
    // of them silently extends the file's list instead of replacing it.
    for (auto const& spelling: { std::vector<char const*> { "--item=argv" }, std::vector<char const*> { "--item", "argv" } })
    {
        Sample result;
        result.items = { "file-a" };
        std::span<char const* const> const argv { spelling };
        ClearListsNamedOn(SampleOptions(), argv, result);
        INFO("first token: " << spelling.front());
        CHECK(result.items.empty());
    }
}

TEST_CASE("FileOptions: a list flag appearing as another flag's VALUE does not clear", "[config][file]")
{
    // `--name --item` gives `--name` the value "--item". A reset that scanned every
    // token for a list spelling would read that value as naming the list and empty
    // what the file declared -- silently, and in the direction that loses settings.
    // The value is consumed here through the parser's own `TakeValue`, so the two
    // agree by construction rather than by two authors reading the same rule.
    Sample result;
    result.items = { "file-a" };

    auto const args = std::to_array<char const*>({ "--name", "--item" });
    std::span<char const* const> const argv { args };
    ClearListsNamedOn(SampleOptions(), argv, result);

    CHECK(result.items == std::vector<std::string> { "file-a" });

    // And the attached form of the same shape: `--name=--item` was never at risk,
    // which is what makes the separate-argument form the one worth pinning.
    Sample attached;
    attached.items = { "file-a" };
    auto const attachedArgs = std::to_array<char const*>({ "--name=--item" });
    ClearListsNamedOn(SampleOptions(), std::span<char const* const> { attachedArgs }, attached);
    CHECK(attached.items == std::vector<std::string> { "file-a" });
}
