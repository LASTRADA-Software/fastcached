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
#include <iterator>
#include <ranges>
#include <regex>
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
/// seven defects and misses `tools.md:1099`, whose only fault was a missing
/// `--scheduler` -- refused by an inline `if` in `main.cpp` rather than by the table.
///
/// That gap is **a bug in the table, not a boundary of this check**
/// ([#386](https://github.com/LASTRADA-Software/fastcached/issues/386)): the rule
/// depends on nothing but the parsed configuration, which is precisely what
/// `platform-service-and-config.md` says belongs in a table "never in the tier that
/// happens to need it". Its install-time twin already IS a row, so the two agree by
/// coincidence rather than by construction. When #386 lands this check gets wider
/// for free and no line here changes. It is left open only because the fix collides
/// with #403 in `main.cpp`, not because it is settled.
///
/// Whoever moves that row: it belongs in `StartupPolicyRejection`, **not** in
/// `NodeServiceRejection`, which #386 as filed names. That one is install-time only
/// -- its sole production caller is `NodeInstallRejection` and its messages say
/// "required *to install a service*" -- so deleting the inline `if` in favour of it
/// would let a node START with an empty `--scheduler`, which is the refusal the
/// ticket exists to protect.
///
/// **Anything past the startup policy**: a flag whose *value* is wrong in a way only
/// a running node discovers, a path that does not exist, a port already held. Those
/// are properties of a machine rather than of a command line, and a check that tried
/// to have opinions about them would fail on every developer's box for reasons the
/// documentation cannot fix.
///
/// **This whole binary is gated on `FASTCACHED_BUILD_NODE`.** Configure with it
/// `OFF` and this check silently stops running while `node-config-reference` carries
/// on -- so a docs-only change validated in such a tree is validated by less than it
/// looks. That is the same shape as the sweep-scope trap in
/// `build-and-toolchain.md`: a check is only as complete as the target set that
/// built it.
namespace
{

/// A documentation page whose command lines are NOT checked.
///
/// The pages are **globbed** rather than listed, and this is the exclusion table.
/// The direction matters and it is the whole lesson of this ticket: a hand-written
/// include list silently omits, and what it omits is a page nobody re-ran. Two pages
/// with real node invocations -- `operations/cluster-communication.md` and
/// `operations/upgrading-a-store.md` -- were invisible to the first draft of this
/// check for exactly that reason. Globbing inverts the failure: a new page with a
/// broken example fails loudly once, instead of never being covered at all.
///
/// Matched as a path SUBSTRING, so a row survives a file being moved between
/// directories but not renamed.
struct ExcludedPage
{
    std::string_view needle; ///< A substring of the page's path, using '/' separators.
    std::string_view why;    ///< Why its command lines are not startup configurations.
};

/// Pages deliberately outside the check.
///
/// Empty is legal here and is not the vacuity hazard `SkippedExamples` guards: an
/// empty exclusion list means every page is checked, which is the strong position,
/// whereas an empty *skip* list would mean the check had stopped looking.
constexpr std::array<ExcludedPage, 0> ExcludedPages {};

/// A verb that ends the process before the startup gate is ever consulted.
///
/// `main.cpp` handles these and `return`s: `--help` / `--version` before the
/// configuration is even assembled, then `--print-surfaces` (:1563),
/// `--install-service` / `--uninstall-service` (:1596, judged by the stricter
/// `NodeInstallRejection` instead), `--migrate-cache` (:1637) and ALL FOUR
/// `cluster.action` verbs (:1653) -- `--cluster-set` included, which is easy to
/// leave out because it is the one of the four the admin prose demonstrates last.
/// A command line naming one of them is not a *start*, so asking whether it would
/// start is asking the wrong question of it.
///
/// `--print-surfaces` is the sharp case and the reason this table exists. Its
/// short-circuit is deliberate -- an operator reaches for it *because* a port is
/// wrong, and withholding the map until the configuration is valid withholds it
/// exactly when it is wanted. The first draft of this check did not know that, so
/// the documented example failed it, and five flags were added to the page one at a
/// time to appease it. Each satisfied one more row of a gate the binary skips for
/// that verb; not one changed a line of the output block beneath it; and the page
/// ended up teaching that `--print-surfaces` demands a scheduler and a cluster key,
/// which is false. The edit was reverted. **A fix that needs several rounds of
/// appeasing a checker, against a page whose expected output never moves, is the
/// checker asking the wrong question.**
///
/// Unlike `SkippedExamples`, a row here that matches nothing is NOT a failure, and
/// the difference is where the row comes from: these are derived from what the
/// *binary* short-circuits on, so a verb with no documented example yet is ordinary.
/// A skip row, by contrast, names one specific example that must still exist.
struct NonStartVerb
{
    std::string_view flag; ///< The flag whose presence ends the process early.
    std::string_view why;  ///< What it does instead of starting.
};

constexpr std::array NonStartVerbs {
    NonStartVerb { .flag = "--print-surfaces",
                   .why = "prints the resolved surface map and exits, deliberately ahead of the startup rules, "
                          "because it is reached for when a port is wrong" },
    NonStartVerb { .flag = "--install-service",
                   .why = "registers a service and exits; judged by NodeInstallRejection, which is stricter" },
    NonStartVerb { .flag = "--uninstall-service", .why = "removes a registration and exits" },
    NonStartVerb { .flag = "--migrate-cache", .why = "converts a store and exits" },
    NonStartVerb { .flag = "--cluster-status", .why = "a cluster admin verb: asks the leader and exits" },
    NonStartVerb { .flag = "--cluster-set", .why = "a cluster admin verb: changes one replicated setting and exits" },
    NonStartVerb { .flag = "--cluster-admit", .why = "a cluster admin verb: proposes a member and exits" },
    NonStartVerb { .flag = "--cluster-forget", .why = "a cluster admin verb: proposes a removal and exits" },
    // Answered before the configuration is even assembled, so these are further from
    // a start than anything above them.
    NonStartVerb { .flag = "--help", .why = "prints usage and exits, ahead of the config file being read" },
    NonStartVerb { .flag = "--version", .why = "prints the version and exits, ahead of the config file being read" },
};

/// A command line the check deliberately does not judge.
///
/// Matched on a distinctive SUBSTRING rather than a line number, because a line
/// number in a markdown file moves the moment somebody adds a paragraph above it --
/// and a skip row that silently stops matching is a skip row that starts hiding a
/// real example.
///
/// Every row carries a reason, and a row matching nothing FAILS.
/// `node-config-reference` fails when either of its scans matches nothing for the
/// same reason: two empty lists agree perfectly.
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
/// in. A RULE rather than skip-table rows, because a per-example row for each would
/// go stale every time one was reworded.
/// @param command The command line as written.
/// @return True when it holds an elision or a placeholder.
[[nodiscard]] bool IsTemplate(std::string_view command)
{
    static std::regex const placeholder { R"(\.\.\.|<[A-Za-z-]+>)" };
    return std::regex_search(command.begin(), command.end(), placeholder);
}

/// Whether @p command names a verb that ends the process before the startup gate.
///
/// A range-based `for` rather than `ranges::find_if`, and that is portability rather
/// than taste: an iterator into a `std::array` is a raw POINTER on libstdc++ and
/// libc++ and a CLASS on the MSVC STL, so `readability-qualified-auto` asks for a
/// spelling (`auto const *const`) that only compiles on two of the three. The three
/// lookups in this file each take the same shape for the same reason.
/// @param command The command line as written.
/// @return The row, or nullptr when this command line is a start.
[[nodiscard]] NonStartVerb const* NonStartVerbIn(std::string_view command)
{
    for (auto const& verb: NonStartVerbs)
        if (command.contains(verb.flag))
            return &verb;
    return nullptr;
}

/// The exclusion row covering @p page, if any.
/// @param page A page path, relative to the repository root.
/// @return The row, or nullptr when the page is checked.
[[nodiscard]] ExcludedPage const* ExclusionFor(std::string_view page)
{
    for (auto const& row: ExcludedPages)
        if (page.contains(row.needle))
            return &row;
    return nullptr;
}

/// The skip row covering @p command, if any.
/// @param command A documented command line.
/// @return The row, or nullptr when the command line is checked.
[[nodiscard]] SkippedExample const* SkipFor(std::string_view command)
{
    for (auto const& row: SkippedExamples)
        if (command.contains(row.needle))
            return &row;
    return nullptr;
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
/// Only a `#` that begins a word counts, so a `#` inside a value is left alone --
/// and this runs AFTER any `# ` root prompt is removed, or it would eat the command.
/// Returns a VIEW: the result is always a prefix of @p command, and this runs for
/// every line inside a fence rather than only for the invocations, so an owning
/// return allocated 181 times per run to keep 25 commands. The view is into
/// `pending`, which outlives every statement that reads it.
/// @param command The command line, prompt already stripped.
/// @return The command with any trailing comment removed.
[[nodiscard]] std::string_view StripShellComment(std::string_view command)
{
    for (auto const index: std::views::iota(std::size_t { 0 }, command.size()))
        if (command[index] == '#' && (index == 0 || std::isspace(static_cast<unsigned char>(command[index - 1])) != 0))
            return command.substr(0, index);
    return command;
}

/// Trim ASCII whitespace from both ends.
/// @param text The text.
/// @return The trimmed view.
[[nodiscard]] std::string_view Trim(std::string_view text)
{
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0))
        text.remove_prefix(1);
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0))
        text.remove_suffix(1);
    return text;
}

/// Whether @p text is the node being INVOKED, rather than the node speaking.
///
/// `fastcache-compile-node: this node does not lead the cluster; ask ...` is an
/// error message the binary prints, quoted in a fenced block so a reader recognises
/// it. A `starts_with` on the program name alone matched it and reported the
/// documentation as broken -- the check accusing its subject when the instrument was
/// wrong, which is the failure this whole file exists to prevent one level down. The
/// colon is what separates the two: a program name followed by anything but
/// whitespace is not a command.
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

/// Every `fastcache-compile-node` invocation inside a fenced block of @p page.
///
/// Backslash continuations are joined, and the reported line is the one the command
/// STARTS on, because that is where a reader looks.
/// @param root The repository root.
/// @param page The page's path, relative to the root.
/// @return The commands, in document order.
[[nodiscard]] std::vector<FoundCommand> CommandsIn(std::filesystem::path const& root, std::string const& page)
{
    std::ifstream in { root / page };
    REQUIRE(in.is_open());

    std::vector<FoundCommand> found;
    std::string line;
    bool inFence = false;
    int number = 0;
    std::string pending;
    int pendingLine = 0;

    while (std::getline(in, line))
    {
        ++number;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // A fence ends whatever was accumulating: an unterminated continuation
        // inside a block is a documentation typo, not something to carry across.
        //
        // **Trimmed, because a fence this project uses is frequently INDENTED.** A
        // `=== "One machine"` content tab and a `!!! note` admonition both carry
        // their fences four spaces in, and a `starts_with("```")` on the raw line
        // never sees them -- so `inFence` stays false for the whole block and every
        // command inside it is dropped in silence. That is the exact failure this
        // file's header claims globbing inverted: the page was scanned, the example
        // was not, and nothing said so. It was not hypothetical either --
        // `operations/cluster-communication.md`'s "One machine" tab held a command
        // line the node refuses to start on while this check reported green.
        if (Trim(line).starts_with("```"))
        {
            inFence = !inFence;
            pending.clear();
            continue;
        }
        if (!inFence)
            continue;

        if (pending.empty())
            pendingLine = number;
        if (!line.empty() && line.back() == '\\')
        {
            pending += line.substr(0, line.size() - 1);
            pending += ' ';
            continue;
        }
        pending += line;

        // The prompt comes off BEFORE the comment, or a `# ` root prompt is read as
        // a comment and the whole command silently disappears.
        auto text = Trim(pending);
        for (auto const prompt: { std::string_view { "$ " }, std::string_view { "# " } })
            if (text.starts_with(prompt))
                text.remove_prefix(prompt.size());
        if (auto const command = Trim(StripShellComment(text)); IsInvocation(command))
            found.push_back(FoundCommand { .page = page, .line = pendingLine, .command = std::string { command } });
        pending.clear();
    }
    return found;
}

/// Every markdown page under `docs/`, relative to @p root, in a stable order.
/// @param root The repository root.
/// @return The pages, using '/' separators.
[[nodiscard]] std::vector<std::string> DocumentationPages(std::filesystem::path const& root)
{
    std::vector<std::string> pages;
    for (auto const& entry: std::filesystem::recursive_directory_iterator { root / "docs" })
        if (entry.is_regular_file() && entry.path().extension() == ".md")
            pages.push_back(entry.path().lexically_relative(root).generic_string());
    std::ranges::sort(pages);
    return pages;
}

} // namespace

TEST_CASE("Every documented command line is one the node would start on", "[node][docs][config]")
{
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };

    auto const pages = DocumentationPages(root);
    // A tree with no documentation is this check examining nothing while agreeing
    // with everything -- the shape its own subject had.
    REQUIRE_FALSE(pages.empty());

    std::vector<FoundCommand> commands;
    std::vector<std::string> excludedSeen;
    for (auto const& page: pages)
    {
        if (auto const* excluded = ExclusionFor(page); excluded != nullptr)
        {
            excludedSeen.emplace_back(excluded->needle);
            continue;
        }
        auto found = CommandsIn(root, page);
        commands.insert(commands.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
    }

    std::vector<std::string> skipsUsed;
    std::size_t checked = 0;
    std::size_t templates = 0;
    std::size_t nonStart = 0;
    std::size_t failures = 0;

    for (auto const& found: commands)
    {
        if (IsTemplate(found.command))
        {
            ++templates;
            continue;
        }

        if (auto const* verb = NonStartVerbIn(found.command); verb != nullptr)
        {
            ++nonStart;
            continue;
        }

        if (auto const* skip = SkipFor(found.command); skip != nullptr)
        {
            skipsUsed.emplace_back(skip->needle);
            continue;
        }

        ++checked;
        auto const arguments = SplitArguments(found.command);
        // The program name is dropped: `ParseOptionsInto` takes flags only.
        auto flags = arguments | std::views::drop(1)
                     | std::views::transform([](std::string const& argument) { return argument.c_str(); });
        std::vector<char const*> argv { flags.begin(), flags.end() };

        NodeConfig cfg;
        auto const parsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
        if (!parsed.has_value())
        {
            ++failures;
            FAIL_CHECK(std::format(
                "{}:{}: does not parse: {}\n    {}", found.page, found.line, parsed.error().ToString(), found.command));
            continue;
        }

        if (auto const rejection = StartupPolicyRejection(cfg); rejection.has_value())
        {
            ++failures;
            // The refusal is CORRECT. What is wrong is documentation telling a
            // reader to run a configuration the binary refuses (#563).
            FAIL_CHECK(std::format("{}:{}: parses, and the node would refuse to start:\n    {}\n    {}",
                                   found.page,
                                   found.line,
                                   *rejection,
                                   found.command));
        }
    }

    // Counts first, so they are in scope for every assertion below rather than
    // trailing them where Catch2 would never print them.
    INFO("scanned " << pages.size() << " page(s); found " << commands.size() << " command line(s); checked " << checked
                    << "; templates " << templates << "; non-start verbs " << nonStart << "; failures " << failures);

    // A skip row that matches nothing has stopped describing anything, and would sit
    // there looking like coverage while hiding whatever moved into its place.
    for (auto const& row: SkippedExamples)
    {
        INFO("skip row: " << row.why);
        CHECK(std::ranges::contains(skipsUsed, row.needle));
    }
    for (auto const& row: ExcludedPages)
    {
        INFO("excluded page: " << row.why);
        CHECK(std::ranges::contains(excludedSeen, row.needle));
    }

    // The vacuity guard. A scan that examined nothing agrees with every
    // documentation there could be.
    CHECK(checked > 0);

    // Restates the per-command failures above as one assertion, so the INFO in scope
    // here -- what the check actually examined -- reaches the report. A `FAIL_CHECK`
    // inside the loop cannot carry it: the counts are not final at that point. A
    // summary nobody ever sees is the dead-INFO defect this file was itself written
    // to avoid one level down.
    CHECK(failures == 0);
}
