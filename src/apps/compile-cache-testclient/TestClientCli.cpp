// SPDX-License-Identifier: Apache-2.0
#include "TestClientCli.hpp"

#include <FastCache/Config/CliParser.hpp>

#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <vector>

namespace FastCache::TestClient
{

namespace
{
    /// One sub-command: the positional first token.
    struct SubCommand
    {
        std::string_view name; ///< As typed.
        Action action;         ///< What it selects.
        std::string_view summary;
    };

    /// The sub-commands, in the order `--help` documents them.
    constexpr auto SubCommands = std::to_array<SubCommand>({
        { .name = "store",
          .action = Action::Store,
          .summary = "Compile <source>, frame the object plus its /showIncludes\n"
                     "region, and STORE it under <key>." },
        { .name = "fetch",
          .action = Action::Fetch,
          .summary = "FETCH <key> and validate the object and include paths\n"
                     "that come back." },
    });

    /// The accepted options.
    constexpr auto Options = std::to_array<OptionSpec<Args>>({
        { .primary = "--host",
          .arity = Arity::Value,
          .operand = " <addr>",
          .apply = AssignFrom<&Args::host, ParseText>(),
          .description = "daemon host (default 127.0.0.1)" },
        { .primary = "--port",
          .arity = Arity::Value,
          .operand = " <num>",
          .apply = AssignFrom<&Args::port, ParsePort>(),
          .description = "daemon port; required" },
        { .primary = "--key",
          .arity = Arity::Value,
          .operand = " <k>",
          .apply = AssignFrom<&Args::key, ParseText>(),
          .description = "cache key to store under or fetch" },
        { .primary = "--prefetch-group",
          .arity = Arity::Value,
          .operand = " <id>",
          .apply = AssignFrom<&Args::prefetchGroup, ParseText>(),
          .description = "prefetch group id (default 'default')" },
        { .primary = "--srcroot",
          .arity = Arity::Value,
          .operand = " <path>",
          .apply = AssignFrom<&Args::srcRoot, ParseText>(),
          .description = "checkout source root, for path canonicalization" },
        { .primary = "--buildtree",
          .arity = Arity::Value,
          .operand = " <path>",
          .apply = AssignFrom<&Args::buildTree, ParseText>(),
          .description = "build output root" },
        { .primary = "--compiler",
          .arity = Arity::Value,
          .operand = " <compiler>",
          .apply = AssignFrom<&Args::compiler, ParseText>(),
          .description = "compiler to drive; MSVC and GNU driver spellings are\n"
                         "both understood (default cl on Windows, cc elsewhere)" },
        { .primary = "--source",
          .arity = Arity::Value,
          .operand = " <file>",
          .apply = AssignFrom<&Args::source, ParseText>(),
          .description = "source file to compile (store)" },
        { .primary = "--out",
          .arity = Arity::Value,
          .operand = " <obj>",
          .apply = AssignFrom<&Args::object, ParseText>(),
          .description = "object path: output for store, expected-write for fetch" },
        { .primary = "--help",
          .alias = "-h",
          .select = SelectOutcome<&Args::action, Action::ShowHelp>(),
          .flow = ParseFlow::Stop,
          .description = "show this help and exit" },
    });

    static_assert(TableIsWellFormed<Args>(Options),
                  "test client option table is malformed: a row is undocumented, a value flag has no operand, "
                  "a row does nothing, or a spelling is claimed twice");

    /// The sub-command `name` selects, if any.
    ///
    /// By VALUE rather than by iterator, and that is not a style choice: the
    /// iterator of a `std::array` is a raw pointer on libstdc++ and libc++ and a
    /// class type on MSVC, so no single spelling of `auto` compiles everywhere --
    /// `auto const` is what MSVC needs and what clang-tidy's
    /// `readability-qualified-auto` rejects, and `auto const* const` is the reverse
    /// and does not compile with MSVC at all. Returning the row sidesteps the
    /// disagreement instead of picking a side of it. (The neighbouring lookup over
    /// `TestClientOptions()` has no such problem: that is a `std::span`, whose
    /// iterator is a class type on all three.)
    /// @param name The token as typed.
    /// @return The matching row, or nullopt.
    [[nodiscard]] constexpr std::optional<SubCommand> FindSubCommand(std::string_view name)
    {
        for (auto const& command: SubCommands)
            if (command.name == name)
                return command;
        return std::nullopt;
    }

    /// Render one sub-command's invocation form.
    /// @param command The sub-command.
    /// @return The left column of its usage line.
    [[nodiscard]] std::string RenderSubCommand(SubCommand const& command)
    {
        return std::format("compile-cache-testclient {} [options]", command.name);
    }
} // namespace

std::span<OptionSpec<Args> const> TestClientOptions() noexcept
{
    return Options;
}

std::expected<Args, ConfigError> ParseArgs(std::span<char const* const> argv)
{
    if (argv.empty())
        return std::unexpected(
            ArgvError(ConfigErrorCode::MissingRequired, "sub-command", "expected store|fetch (try --help)"));

    // A leading `--help` is a query, not a malformed store. Matched against the
    // table rather than re-spelling "--help"/"-h" here, so the accepted
    // spellings stay in one place; only a row that ends parsing (help) means
    // anything before a sub-command has been chosen.
    std::string_view const head { argv.front() };
    auto const query = std::ranges::find_if(TestClientOptions(), [head](OptionSpec<Args> const& spec) {
        return spec.flow == ParseFlow::Stop && Matches(head, spec);
    });
    if (query != std::ranges::end(TestClientOptions()))
        return Args { .action = Action::ShowHelp };

    auto const command = FindSubCommand(head);
    if (!command.has_value())
        return std::unexpected(
            ArgvError(ConfigErrorCode::UnknownKey, std::string { head }, "unknown sub-command (expected store|fetch)"));

    Args parsed { .action = command->action };
    auto const flow = ParseOptionsInto(TestClientOptions(), argv.subspan(1), parsed);
    if (!flow.has_value())
        return std::unexpected(flow.error());
    // `--help` among the options ends parsing, and a help run has nothing left
    // to satisfy.
    if (*flow == ParseFlow::Stop)
        return parsed;

    // Checked here rather than at the use site so a missing port is one clear
    // diagnostic instead of a connection to port 0.
    if (parsed.port == 0)
        return std::unexpected(ArgvError(ConfigErrorCode::MissingRequired, "--port", "required"));
    return parsed;
}

std::string HelpText(UsageColor color)
{
    UsageRows commandRows;
    for (auto const& command: SubCommands)
        commandRows.Add(RenderSubCommand(command), command.summary);

    UsageRows optionRows;
    AddOptionRows(optionRows, TestClientOptions());

    auto const blocks = std::to_array<UsageBlock>({
        { .entries = commandRows.Rows() },
        { .entries = optionRows.Rows() },
    });

    std::span<UsageBlock const> const allBlocks { blocks };
    auto const sections = std::to_array<UsageSection>({
        { .subject = "compile-cache-testclient - a low-level probe for the 0xFC compile-cache protocol." },
        { .title = "COMMANDS", .blocks = allBlocks.subspan(0, 1) },
        { .title = "OPTIONS", .blocks = allBlocks.subspan(1, 1) },
    });

    return RenderUsage({ .sections = sections }, color);
}

} // namespace FastCache::TestClient
