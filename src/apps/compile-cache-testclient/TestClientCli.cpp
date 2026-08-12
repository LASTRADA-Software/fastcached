// SPDX-License-Identifier: Apache-2.0
#include "TestClientCli.hpp"

#include <FastCache/Config/CliParser.hpp>

#include <array>
#include <cstddef>
#include <format>
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
        { .primary = "--cohort",
          .arity = Arity::Value,
          .operand = " <id>",
          .apply = AssignFrom<&Args::cohort, ParseText>(),
          .description = "prefetch cohort id (default 'default')" },
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
          .operand = " <cl|clang-cl>",
          .apply = AssignFrom<&Args::compiler, ParseText>(),
          .description = "compiler to drive (default cl)" },
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

    // `--help` before any sub-command is a query, not a malformed store.
    std::string_view const head { argv.front() };
    if (head == "--help" || head == "-h")
        return Args { .action = Action::ShowHelp };

    auto const command = std::ranges::find_if(SubCommands, [head](SubCommand const& c) { return c.name == head; });
    if (command == std::ranges::end(SubCommands))
        return std::unexpected(
            ArgvError(ConfigErrorCode::UnknownKey, std::string { head }, "unknown sub-command (expected store|fetch)"));

    Args parsed { .action = command->action };
    auto const rest = argv.subspan(1);
    for (std::size_t i = 0; i < rest.size(); ++i)
    {
        auto const flow = ApplyOneOption(TestClientOptions(), rest, i, parsed);
        if (!flow.has_value())
            return std::unexpected(flow.error());
        if (*flow == ParseFlow::Stop)
            return parsed;
    }

    // Checked here rather than at the use site so a missing port is one clear
    // diagnostic instead of a connection to port 0.
    if (parsed.port == 0)
        return std::unexpected(ArgvError(ConfigErrorCode::MissingRequired, "--port", "required"));
    return parsed;
}

std::string HelpText(UsageColor color)
{
    // Every owning container is filled to completion before any span over it is
    // taken: a later push_back would reallocate and dangle the document's views.
    std::vector<std::string> forms;
    forms.reserve(SubCommands.size() + Options.size());
    for (auto const& command: SubCommands)
        forms.push_back(RenderSubCommand(command));
    for (auto const& spec: Options)
        forms.push_back(RenderFlagForms(spec));

    std::vector<UsageEntry> commandRows;
    commandRows.reserve(SubCommands.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, SubCommands.size()))
        commandRows.push_back({ .term = forms[index], .description = SubCommands[index].summary });

    std::vector<UsageEntry> optionRows;
    optionRows.reserve(Options.size());
    for (auto const index: std::views::iota(std::size_t { 0 }, Options.size()))
        optionRows.push_back({ .term = forms[SubCommands.size() + index], .description = Options[index].description });

    auto const blocks = std::to_array<UsageBlock>({
        { .entries = commandRows },
        { .entries = optionRows },
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
