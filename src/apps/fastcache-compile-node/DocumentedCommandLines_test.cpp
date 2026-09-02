// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"

#include <FastCache/Cli/Options.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Node;

/// Every `fastcache-compile-node` command line the documentation shows must be one
/// the binary would actually start on.
///
/// ## Why this exists
///
/// Six documented command lines exited at startup rather than starting (#563), two
/// of them the whole of the getting-started page's "Setting it up". A reader
/// following the page hit a refusal on their first attempt, with nothing to say the
/// document was wrong rather than their environment. Four more sat on
/// `docs/tools/fastcache-compile-node.md` -- the page #463 had already fixed once
/// for the identical shape, which is what says the examples were written before the
/// refusals and never re-run.
///
/// Nothing connected the two. The option table decides what starts; the prose is
/// prose. `node-config-reference` already covers exactly this gap for the shipped
/// reference *configuration*, and the ticket's own suggestion was that the idea
/// works and wants extending. This is that, one layer in.
///
/// ## Why it does not run the binary
///
/// Spawning would need a free port, a key file that exists, and a machine with no
/// service already on the documented ports -- and it still could not see a
/// **default**, because a default never appears in argv. The survey that found
/// these six failed on precisely that: a command line relying on the loopback
/// default collided with a running service and reported a defect that was not one.
/// Parsing with the real `NodeOptions()` and running the real
/// `StartupPolicyRejection` asks the same question with none of that, and asks it
/// about the whole configuration rather than the part that happened to be spelled
/// out.
///
/// ## What it cannot see, measured rather than assumed
///
/// **Refusals that are not in the table.** This runs `StartupPolicyRejection`, so a
/// cross-flag rule enforced anywhere else is invisible to it. That is not
/// hypothetical: run against the pre-fix documentation this check reports SIX of the
/// seven defects, and misses `tools.md:1099` entirely, because `--scheduler is
/// required` is refused in `main.cpp` rather than by the table
/// ([#585](https://github.com/LASTRADA-Software/fastcached/issues/585)). Its
/// install-time twin IS a table row, which is how the two came to disagree. So the
/// check's coverage is exactly the table's, and it grows when the table does.
///
/// That is also why the six were verified by RUNNING them before this existed. A
/// table check and a spawn answer different questions, and the one that found the
/// seventh defect was the spawn.
///
/// **Anything past the startup policy**: a flag whose *value* is wrong in a way only
/// a running node discovers, a path that does not exist, a port already held. Those
/// are properties of a machine rather than of a command line, and a check that tried
/// to have opinions about them would fail on every developer's box for reasons the
/// documentation cannot fix.
namespace
{

/// A documentation page whose command lines are checked.
///
/// A table rather than a glob: a page added here is a decision, and a glob would
/// silently start checking a page whose examples are deliberately illustrative --
/// which is a check that grows opinions nobody agreed to.
struct DocumentedPage
{
    std::string_view path; ///< Relative to the repository root.
    std::string_view why;  ///< What this page's examples are for.
};

constexpr std::array DocumentedPages {
    DocumentedPage { .path = "docs/getting-started/distributed-compilation.md",
                     .why = "the page a reader follows first; its two central commands both exited (#563)" },
    DocumentedPage { .path = "docs/tools/fastcache-compile-node.md",
                     .why = "the node's reference page, and the one #463 already fixed once for this shape" },
};

/// A command line the check deliberately does not judge.
///
/// Matched on a distinctive SUBSTRING rather than a line number, because a line
/// number in a markdown file moves the moment somebody adds a paragraph above it --
/// and a skip row that silently stops matching is a skip row that starts hiding a
/// real example.
///
/// Every row carries a reason, and both halves of the table are asserted below: a
/// row that matches nothing fails, and an empty table fails. `node-config-reference`
/// fails when either of its scans matches nothing for the same reason -- two empty
/// lists agree perfectly.
struct SkippedExample
{
    std::string_view needle; ///< A substring identifying the command line.
    std::string_view why;    ///< Why it is not a deployment to be checked.
};

constexpr std::array SkippedExamples {
    SkippedExample { .needle = "--log-level=debug --log-timestamps",
                     .why = "illustrates the timestamp PREFIX and is followed by the line it produces; it is not a "
                            "deployment, and bolting --scheduler and a key onto it to make it startable would teach "
                            "the wrong thing about the flag it exists to document" },
};

/// Whether @p command is a template rather than a command.
///
/// `--scheduler=...` and `--advertise=<host>:<port>` are shapes shown to be filled
/// in. This is a RULE rather than a skip-table row because it is a property of the
/// text -- a per-example row for each would go stale every time one was reworded.
/// @param command The command line as written.
/// @return True when it holds an elision or a placeholder.
[[nodiscard]] bool IsTemplate(std::string_view command)
{
    if (command.find("...") != std::string_view::npos)
        return true;
    // `<host>`, `<port>`, `<id>` -- an angle-bracketed lowercase word.
    for (auto i = command.find('<'); i != std::string_view::npos; i = command.find('<', i + 1))
    {
        auto const close = command.find('>', i);
        if (close == std::string_view::npos)
            break;
        auto const inner = command.substr(i + 1, close - i - 1);
        if (!inner.empty() && std::ranges::all_of(inner, [](char c) {
                return (std::isalpha(static_cast<unsigned char>(c)) != 0) || c == '-';
            }))
            return true;
    }
    return false;
}

/// One command line found in the documentation.
struct FoundCommand
{
    std::string page;    ///< Relative path, for the failure message.
    int line {};         ///< 1-based line the command starts on.
    std::string command; ///< The joined command line, continuations resolved.
};

/// Strip a trailing shell comment.
///
/// `--service-scope=user   # macOS: registers a launchd agent` is one argument and
/// a comment to `sh`, and the survey that preceded this check passed the `#` to the
/// parser and reported an unrecognised argument that no reader would ever hit.
/// Only a `#` that begins a word counts, so a `#` inside a value is left alone.
/// @param command The command line as written.
/// @return The command with any trailing comment removed.
[[nodiscard]] std::string StripShellComment(std::string_view command)
{
    for (std::size_t i = 0; i < command.size(); ++i)
        if (command[i] == '#' && (i == 0 || std::isspace(static_cast<unsigned char>(command[i - 1])) != 0))
            return std::string { command.substr(0, i) };
    return std::string { command };
}

/// Split a command line on whitespace, honouring single and double quotes.
/// @param command The command line.
/// @return Its arguments.
[[nodiscard]] std::vector<std::string> SplitArguments(std::string_view command)
{
    std::vector<std::string> out;
    std::string current;
    char quote = '\0';
    auto flush = [&out, &current] {
        if (!current.empty())
        {
            out.push_back(current);
            current.clear();
        }
    };

    for (char const c: command)
    {
        if (quote != '\0')
        {
            if (c == quote)
                quote = '\0';
            else
                current.push_back(c);
        }
        else if (c == '\'' || c == '"')
            quote = c;
        else if (std::isspace(static_cast<unsigned char>(c)) != 0)
            flush();
        else
            current.push_back(c);
    }
    flush();
    return out;
}

/// Whether @p text is the node being INVOKED, rather than the node speaking.
///
/// `fastcache-compile-node: this node does not lead the cluster; ask ...` is an
/// error message the binary prints, quoted in a fenced block so a reader
/// recognises it. A `starts_with` on the program name alone matched it and
/// reported the documentation as broken -- the check accusing its subject when
/// the instrument was wrong, which is the failure this whole file exists to
/// prevent one level down. The colon is what separates the two: a program name
/// followed by anything but whitespace is not a command.
/// @param text A trimmed line from a fenced block.
/// @return True when it invokes the node.
[[nodiscard]] bool IsInvocation(std::string_view text)
{
    constexpr std::string_view Program = "fastcache-compile-node";
    if (!text.starts_with(Program))
        return false;
    auto const rest = text.substr(Program.size());
    return rest.empty() || (std::isspace(static_cast<unsigned char>(rest.front())) != 0);
}

/// Every `fastcache-compile-node` invocation inside a fenced block of @p path.
///
/// Backslash continuations are joined, and the reported line is the one the command
/// STARTS on, because that is where a reader looks.
/// @param root The repository root.
/// @param page The page's relative path.
/// @return The commands, in document order.
[[nodiscard]] std::vector<FoundCommand> CommandsIn(std::filesystem::path const& root, std::string_view page)
{
    std::ifstream in { root / page };
    REQUIRE(in.is_open());

    std::vector<FoundCommand> found;
    std::string line;
    bool inFence = false;
    int number = 0;
    std::string pending;
    int pendingLine = 0;

    /// Take whatever `pending` holds as a complete command.
    auto take = [&found, &pending, &pendingLine, page] {
        auto const stripped = StripShellComment(pending);
        auto trimmed = std::string_view { stripped };
        while (!trimmed.empty() && (std::isspace(static_cast<unsigned char>(trimmed.front())) != 0))
            trimmed.remove_prefix(1);
        while (!trimmed.empty() && (std::isspace(static_cast<unsigned char>(trimmed.back())) != 0))
            trimmed.remove_suffix(1);
        for (auto const prefix: { std::string_view { "$ " }, std::string_view { "# " } })
            if (trimmed.starts_with(prefix))
                trimmed.remove_prefix(prefix.size());
        if (IsInvocation(trimmed))
            found.push_back(FoundCommand { .page = std::string { page },
                                           .line = pendingLine,
                                           .command = std::string { trimmed } });
        pending.clear();
        pendingLine = 0;
    };

    while (std::getline(in, line))
    {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.starts_with("```"))
        {
            if (!pending.empty())
                take();
            inFence = !inFence;
            continue;
        }
        if (!inFence)
            continue;

        if (pendingLine == 0)
            pendingLine = number;
        if (!line.empty() && line.back() == '\\')
        {
            pending += line.substr(0, line.size() - 1);
            pending += ' ';
            continue;
        }
        pending += line;
        take();
    }
    if (!pending.empty())
        take();
    return found;
}

/// The repository root, handed in by CMake.
[[nodiscard]] std::filesystem::path RepositoryRoot()
{
    return std::filesystem::path { FASTCACHED_SOURCE_DIR };
}

} // namespace

TEST_CASE("Every documented command line is one the node would start on", "[node][docs][config]")
{
    auto const root = RepositoryRoot();

    std::vector<std::string> failures;
    std::vector<bool> skipUsed(SkippedExamples.size(), false);
    std::size_t checked = 0;
    std::size_t templates = 0;

    for (auto const& page: DocumentedPages)
    {
        auto const commands = CommandsIn(root, page.path);
        INFO("page " << page.path << " -- " << page.why);
        // A page contributing nothing is a renamed or moved file, and the check
        // would then pass by examining less than it claims.
        CHECK_FALSE(commands.empty());

        for (auto const& found: commands)
        {
            if (IsTemplate(found.command))
            {
                ++templates;
                continue;
            }

            auto const skip = std::ranges::find_if(SkippedExamples, [&found](SkippedExample const& row) {
                return found.command.find(row.needle) != std::string::npos;
            });
            if (skip != SkippedExamples.end())
            {
                skipUsed[static_cast<std::size_t>(std::ranges::distance(SkippedExamples.begin(), skip))] = true;
                continue;
            }

            auto const arguments = SplitArguments(found.command);
            std::vector<char const*> argv;
            argv.reserve(arguments.size());
            // The program name is dropped: `ParseOptionsInto` takes flags only.
            for (auto const& argument: std::span { arguments }.subspan(1))
                argv.push_back(argument.c_str());

            ++checked;
            NodeConfig cfg;
            auto const parsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
            if (!parsed.has_value())
            {
                failures.push_back(std::format("{}:{}: does not parse: {}\n    {}",
                                               found.page,
                                               found.line,
                                               parsed.error().ToString(),
                                               found.command));
                continue;
            }

            if (auto const rejection = StartupPolicyRejection(cfg); rejection.has_value())
                failures.push_back(
                    std::format("{}:{}: parses, and the node would refuse to start:\n    {}\n    {}",
                                found.page,
                                found.line,
                                *rejection,
                                found.command));
        }
    }

    // A skip row that matches nothing has stopped describing anything, and would
    // sit there looking like coverage while hiding whatever moved into its place.
    for (auto const index: std::views::iota(std::size_t { 0 }, SkippedExamples.size()))
        if (!skipUsed[index])
            failures.push_back(std::format("the skip row for \"{}\" matched no documented command line; it is stale, "
                                           "and a stale row hides the example that took its place",
                                           SkippedExamples[index].needle));

    // Both halves of the vacuity guard. A table that examined nothing agrees with
    // every documentation there could be.
    CHECK_FALSE(SkippedExamples.empty());
    CHECK(checked > 0);

    if (!failures.empty())
    {
        std::string report =
            std::format("{} documented command line(s) the node would not start on.\nThe refusals below are "
                        "CORRECT -- what is wrong is documentation telling a reader to run a configuration the "
                        "binary refuses (#563).\n\n",
                        failures.size());
        for (auto const& failure: failures)
            report += "  " + failure + "\n\n";
        FAIL(report);
    }

    INFO("checked " << checked << " command line(s), skipped " << templates << " template(s)");
}
