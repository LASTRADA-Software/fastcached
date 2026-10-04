// SPDX-License-Identifier: Apache-2.0
#include "NodeConfig.hpp"
#include "NodeDefaults.hpp"
#include "NodeFirewall.hpp"
#include "NodeIdentity.hpp"
#include "NodeKey.hpp"

#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cluster/FleetPin.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <ranges>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>
#include <tests/FirewallFakes.hpp>
#include <tests/HostNamingFakes.hpp>
#include <tests/NodeFormationFakes.hpp>
#include <tests/Unwrap.hpp>

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
/// configuration is even assembled, then `--install-service` / `--uninstall-service`
/// (judged by the stricter `NodeInstallRejection` instead), `--migrate-cache` and ALL
/// FOUR `cluster.action` verbs -- `--cluster-set` included, which is easy to leave out
/// because it is the one of the four the admin prose demonstrates last. A command line
/// naming one of them is not a *start*, so asking whether it would start is asking the
/// wrong question of it.
///
/// **`--print-surfaces` is NOT one of them any more, and its absence is deliberate.**
/// It prints the map for ANY configuration -- an operator reaches for it *because* a
/// port is wrong, and withholding the map then would withhold it exactly when it is
/// wanted -- and its EXIT CODE is `StartupPolicyRejection`'s verdict (`ReportSurfaces`,
/// #582): a configuration the node would refuse prints the whole map and then exits 2
/// naming the rule. So a documented `--print-surfaces` example is judged here exactly
/// as a start is, because that is the question its exit code answers.
///
/// It was a row, and the row was right when it was written (`git log -S"The edit was
/// reverted"` on this file, f19375a54): the verb then printed and returned 0 whatever
/// the configuration, so five flags added to a page one at a time to appease this
/// check changed nothing the binary did, and that edit was reverted as teaching that
/// `--print-surfaces` demands a scheduler, which was then false. #582 made it true of
/// the exit code while the row stayed, and the row then hid a documented example that
/// exited 2: `docs/tools/fastcache-compile-node.md`'s own, on the page this check was
/// written for. **An exemption
/// whose premise is a property of the binary goes stale when the binary changes, and
/// nothing about the row says so.**
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
    // Judged by nothing here: since the zero-config defaults the state directory comes from the
    // environment, so what refuses this verb -- `main`'s `NoStateDirectoryRefusal` on a machine
    // whose environment names none, or a key file on disk -- is nothing a configuration can say.
    NonStartVerb { .flag = "--print-identity",
                   .why = "prints this node's identity and exits, ahead of the startup rules: it is reached for while "
                          "the members' command lines are still being written, and its exit code does not judge "
                          "them" },
    NonStartVerb { .flag = "--install-service",
                   .why = "registers a service and exits; judged by NodeInstallRejection, which is stricter" },
    NonStartVerb { .flag = "--uninstall-service", .why = "removes a registration and exits" },
    NonStartVerb { .flag = "--migrate-cache", .why = "converts a store and exits" },
    NonStartVerb { .flag = "--cluster-status", .why = "a cluster admin verb: asks the leader and exits" },
    NonStartVerb { .flag = "--cluster-set", .why = "a cluster admin verb: changes one replicated setting and exits" },
    NonStartVerb { .flag = "--cluster-admit", .why = "a cluster admin verb: proposes a member and exits" },
    NonStartVerb { .flag = "--cluster-forget", .why = "a cluster admin verb: proposes a removal and exits" },
    // The enrollment verbs. The three operator ones are the cluster-admin rows above in
    // every respect that matters here -- ask the window's holder one question, print
    // the answer, exit -- and they are listed rather than left out because leaving them
    // out is not neutral: each one carries a `--scheduler` and would therefore be
    // JUDGED as a serving configuration and pass, so the check would be asking the
    // wrong question about them and reporting that it had asked the right one.
    NonStartVerb { .flag = "--enroll-list", .why = "an enrollment verb: prints what is pending and exits" },
    NonStartVerb { .flag = "--enroll-approve", .why = "an enrollment verb: admits one pending id and exits" },
    NonStartVerb { .flag = "--enroll-reject", .why = "an enrollment verb: refuses one pending id and exits" },
    NonStartVerb { .flag = "--enroll-auto-approve",
                   .why = "an enrollment verb: arms or ends the leader's deadline and exits" },
    NonStartVerb { .flag = "--enroll-clear", .why = "an enrollment verb: drops the undecided requests and exits" },
    // Answered before the configuration is even assembled, so these are further from
    // a start than anything above them.
    NonStartVerb { .flag = "--help", .why = "prints usage and exits, ahead of the config file being read" },
    NonStartVerb { .flag = "--version", .why = "prints the version and exits, ahead of the config file being read" },
    NonStartVerb { .flag = "--check-arguments",
                   .why = "parses the command line and exits, ahead of the config file being read" },
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
/// `core::findIfOrNull` rather than `ranges::find_if`, and that is portability rather than
/// taste: an iterator into a `std::array` is a raw POINTER on libstdc++ and libc++
/// and a CLASS on the MSVC STL, so `readability-qualified-auto` asks for a spelling
/// (`auto const *const`) that only compiles on two of the three. The helper resolves
/// that once, inside a template; its header carries the full argument. The three
/// lookups in this file each take the same shape for the same reason.
/// @param command The command line as written.
/// @return The row, or nullptr when this command line is a start.
[[nodiscard]] NonStartVerb const* NonStartVerbIn(std::string_view command)
{
    return core::findIfOrNull(NonStartVerbs, [command](NonStartVerb const& verb) { return command.contains(verb.flag); });
}

/// The exclusion row covering @p page, if any.
/// @param page A page path, relative to the repository root.
/// @return The row, or nullptr when the page is checked.
[[nodiscard]] ExcludedPage const* ExclusionFor(std::string_view page)
{
    return core::findIfOrNull(ExcludedPages, [page](ExcludedPage const& row) { return page.contains(row.needle); });
}

/// The skip row covering @p command, if any.
/// @param command A documented command line.
/// @return The row, or nullptr when the command line is checked.
[[nodiscard]] SkippedExample const* SkipFor(std::string_view command)
{
    return core::findIfOrNull(SkippedExamples,
                              [command](SkippedExample const& row) { return command.contains(row.needle); });
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

        if (auto const* skip = SkipFor(found.command); skip != nullptr)
        {
            skipsUsed.emplace_back(skip->needle);
            continue;
        }

        // **Every non-template command line is PARSED, whether or not it starts a
        // node.** A non-start verb short-circuits the STARTUP GATE and nothing else:
        // `--cluster-admit` and `--enroll-list` have an argv exactly as parseable as a
        // serving node's, and a flag that does not exist is a documentation defect on
        // either. This test ran the two skips together until the enrollment verbs were
        // added, which is when it mattered enough to measure -- a typo in any of the
        // eleven `--cluster-*` / `--print-surfaces` / `--help` examples was invisible,
        // and adding six rows to `NonStartVerbs` would have taken five more command
        // lines out of every check rather than into the right one. **Measured both
        // ways**: with the skip above the parse, planting `--not-a-real-flag` in a
        // documented `--enroll-list` line leaves this case GREEN; with it here, the
        // same plant fails it by name and line. Whole-suite cost of the move: nothing
        // -- all sixteen parse today.
        auto const arguments = SplitArguments(found.command);
        // The program name is dropped: `ParseOptionsInto` takes flags only.
        auto flags = arguments | std::views::drop(1)
                     | std::views::transform([](std::string const& argument) { return argument.c_str(); });
        std::vector<char const*> argv { flags.begin(), flags.end() };

        // Shaped as `main` shapes a first start before judging it: the record that start mints.
        auto cfg = Testing::FirstStart(NodeConfig {});
        auto const parsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
        if (!parsed.has_value())
        {
            ++failures;
            FAIL_CHECK(std::format(
                "{}:{}: does not parse: {}\n    {}", found.page, found.line, parsed.error().ToString(), found.command));
            continue;
        }

        if (auto const* verb = NonStartVerbIn(found.command); verb != nullptr)
        {
            ++nonStart;
            continue;
        }

        // Counted here rather than above the parse, so `checked` goes on meaning what
        // its name says -- command lines judged against the startup gate -- now that
        // parsing reaches strictly more of them than the gate does.
        ++checked;
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

namespace
{

/// The MSI's WiX fragment, `packaging/windows/service-actions.xml`, whole.
/// @param root The repository root.
/// @return Its text.
[[nodiscard]] std::string MsiFragmentText(std::filesystem::path const& root)
{
    std::ifstream in { root / "packaging" / "windows" / "service-actions.xml", std::ios::binary };
    REQUIRE(in.good());
    // A sized read through the buffer, the tree's spelling (`istreambuf-iterator`).
    std::ostringstream text;
    text << in.rdbuf();
    return std::move(text).str();
}

/// The command line the MSI runs to register the node, as `packaging/windows/service-actions.xml`
/// spells it, with its one XML entity decoded and every `[PROPERTY]` still unformatted.
/// @param root The repository root.
/// @return The `ExeCommand` of the `FastCacheNodeInstallService` action.
[[nodiscard]] std::string MsiNodeInstallCommand(std::filesystem::path const& root)
{
    auto const text = MsiFragmentText(root);
    auto const action = text.find(R"(<CustomAction Id="FastCacheNodeInstallService")");
    REQUIRE(action != std::string::npos);
    constexpr std::string_view Attribute = R"(ExeCommand=")";
    auto const attribute = text.find(Attribute, action);
    REQUIRE(attribute != std::string::npos);
    auto const valueStart = attribute + Attribute.size();
    auto const valueEnd = text.find('"', valueStart);
    REQUIRE(valueEnd != std::string::npos);
    auto command = text.substr(valueStart, valueEnd - valueStart);
    constexpr std::string_view Quote = "&quot;";
    auto at = command.find(Quote);
    while (at != std::string::npos)
    {
        command.replace(at, Quote.size(), "\"");
        at = command.find(Quote, at + 1);
    }
    return command;
}

/// @p command with every `[property]` Windows Installer would format replaced by @p value.
/// @param command An `ExeCommand`.
/// @param property The property's name.
/// @param value What the transaction set it to; empty for a property it did not set.
/// @return The formatted command.
[[nodiscard]] std::string Formatted(std::string command, std::string_view property, std::string_view value)
{
    auto const placeholder = std::format("[{}]", property);
    auto at = command.find(placeholder);
    while (at != std::string::npos)
    {
        command.replace(at, placeholder.size(), value);
        at = command.find(placeholder, at + value.size());
    }
    return command;
}

/// A state directory that holds no formation record: the first start mints the solitary one.
class NoKeptFormation final: public IKeptFormationReader
{
  public:
    /// @copydoc IKeptFormationReader::Read
    [[nodiscard]] std::expected<KeptFormation, std::string> Read() const override
    {
        return KeptFormation {};
    }
};

/// @p arguments parsed as `main` parses a first start.
/// @param arguments The flags, without the program name.
/// @return The configuration, or the parse error.
[[nodiscard]] std::expected<NodeConfig, ConfigError> ParsedFirstStart(std::span<std::string const> arguments)
{
    std::vector<char const*> argv;
    argv.reserve(arguments.size());
    for (auto const& argument: arguments)
        argv.push_back(argument.c_str());
    auto cfg = Testing::FirstStart(NodeConfig {});
    auto const parsed = ParseOptionsInto(NodeOptions(), std::span<char const* const> { argv }, cfg);
    if (!parsed.has_value())
        return std::unexpected { parsed.error() };
    return cfg;
}

} // namespace

TEST_CASE("The MSI's node registration installs with no property, with an advertised endpoint, and survives the round trip",
          "[node][docs][service][msi]")
{
    // platform-service-and-config.md: whatever reaches a supervisor survives this project's own
    // parser round trip, including the flags the INSTALLER adds. So the command is read out of
    // the MSI source itself and run through the node's parser, its install rules and its
    // registration, then the registration is parsed back -- in every shape the MSI can produce,
    // the first naming NO property: a node that serves is refused `--scheduler`, and the custom
    // action is `Return="ignore"`, so a command still passing one would leave no service while
    // the install SUCCEEDED.
    // `FASTCACHE_NODE_ADVERTISE` is optional, and left out it must leave the registration with no
    // `--advertise` at all, so the node advertises this machine's name at every start. And the
    // MSI names no `--cluster-dir`, so the registration owns the machine-wide state directory and
    // secludes it (`PathPrivacy::Private`) rather than baking a path that outranks the file.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const command = MsiNodeInstallCommand(root);
    CHECK_FALSE(command.contains("--cluster-dir"));
    CHECK_FALSE(command.contains("--scheduler"));

    struct Shape
    {
        std::string_view what;              ///< The transaction, as an operator types it.
        std::string_view advertiseArgument; ///< What `SetFastCacheNodeAdvertiseArgument` leaves.
        std::string_view advertised;        ///< The endpoint the registration must carry; empty for none.
        std::string_view allowArgument;     ///< What `SetFastCacheFirewallAllowArgument` leaves.
        std::string_view allowed;           ///< The firewall scope the install takes; empty for none.
        std::string_view replyPort;         ///< `FASTCACHE_DISCOVERY_REPLY_PORT` after the MSI's default.
        std::string_view replyRule;         ///< The discovery-reply rule the install must open.
        std::string_view seed;              ///< `FASTCACHE_FLEET_SEED`; empty for none.
        std::string_view seeded;            ///< The seed the registration must carry, normalized; empty for none.
        std::string_view fleetId {};        ///< `FASTCACHE_FLEET_ID`, passed VERBATIM; empty for none.
    };
    // The package's default reply port, as the fragment's Property row spells it.
    constexpr std::string_view PackagedReplyPort = "6682";
    constexpr auto Shapes = std::to_array<Shape>({
        { .what = "no property",
          .advertiseArgument = "",
          .advertised = "",
          .allowArgument = "",
          .allowed = "",
          .replyPort = PackagedReplyPort,
          .replyRule = "FastCacheCompileNode discovery-reply udp/6682",
          .seed = "",
          .seeded = "" },
        { .what = "FASTCACHE_NODE_ADVERTISE=worker-01.internal:6674",
          .advertiseArgument = "--advertise=worker-01.internal:6674",
          .advertised = "worker-01.internal:6674",
          .allowArgument = "",
          .allowed = "",
          .replyPort = PackagedReplyPort,
          .replyRule = "FastCacheCompileNode discovery-reply udp/6682",
          .seed = "",
          .seeded = "" },
        { .what = "FASTCACHE_FIREWALL_ALLOW=10.0.0.0/8",
          .advertiseArgument = "",
          .advertised = "",
          .allowArgument = "--firewall-allow=10.0.0.0/8",
          .allowed = "10.0.0.0/8",
          .replyPort = PackagedReplyPort,
          .replyRule = "FastCacheCompileNode discovery-reply udp/6682",
          .seed = "",
          .seeded = "" },
        { .what = "FASTCACHE_DISCOVERY_REPLY_PORT=7000",
          .advertiseArgument = "",
          .advertised = "",
          .allowArgument = "",
          .allowed = "",
          .replyPort = "7000",
          .replyRule = "FastCacheCompileNode discovery-reply udp/7000",
          .seed = "",
          .seeded = "" },
        // A machine across a VPN that no beacon reaches: the one seed the MSI passes, stored and
        // replayed in the parser's own normalized spelling, the node port added.
        { .what = "FASTCACHE_FLEET_SEED=office-a.vpn.example",
          .advertiseArgument = "",
          .advertised = "",
          .allowArgument = "",
          .allowed = "",
          .replyPort = PackagedReplyPort,
          .replyRule = "FastCacheCompileNode discovery-reply udp/6682",
          .seed = "office-a.vpn.example",
          .seeded = "office-a.vpn.example:6674" },
        // A machine pinned to the one fleet it may join: the pin `fastcache-cli node` prints, passed
        // and replayed verbatim -- it is security material, so nothing re-spells it.
        { .what = "FASTCACHE_FLEET_ID=<cluster-id>@<key>",
          .advertiseArgument = "",
          .advertised = "",
          .allowArgument = "",
          .allowed = "",
          .replyPort = PackagedReplyPort,
          .replyRule = "FastCacheCompileNode discovery-reply udp/6682",
          .seed = "",
          .seeded = "",
          .fleetId = "0123456789abcdef0123456789abcdef@11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo" },
    });
    // The default the shapes assume is the one the fragment declares.
    CHECK(std::filesystem::exists(root / "packaging" / "windows" / "service-actions.xml"));
    CHECK(MsiFragmentText(root).contains(
        std::format(R"(<Property Id="FASTCACHE_DISCOVERY_REPLY_PORT" Value="{}")", PackagedReplyPort)));

    auto const machineWide = MachineWideNodeClusterDirectory(Testing::InstallerPathProbe());
    REQUIRE(machineWide.has_value());

    for (auto const& shape: Shapes)
    {
        INFO(shape.what);
        auto formatted = Formatted(command, "INSTALL_ROOT", R"(C:\Program Files\fastcached\)");
        formatted = Formatted(std::move(formatted), "FastCacheNodeAdvertiseArgument", shape.advertiseArgument);
        // Every property the transaction sets, the raw one too: a command that went back to
        // spelling `--advertise=[FASTCACHE_NODE_ADVERTISE]` itself must reach the parser as the
        // empty flag it would be, not stop at the bracket check below.
        formatted = Formatted(std::move(formatted), "FASTCACHE_NODE_ADVERTISE", shape.advertised);
        formatted = Formatted(std::move(formatted), "FastCacheFirewallAllowArgument", shape.allowArgument);
        formatted = Formatted(std::move(formatted), "FASTCACHE_FIREWALL_ALLOW", shape.allowed);
        formatted = Formatted(std::move(formatted),
                              "FastCacheNodeDiscoveryReplyArgument",
                              std::format("--discovery-reply-port={}", shape.replyPort));
        formatted = Formatted(std::move(formatted), "FASTCACHE_DISCOVERY_REPLY_PORT", shape.replyPort);
        formatted = Formatted(std::move(formatted),
                              "FastCacheFleetSeedArgument",
                              shape.seed.empty() ? std::string {} : std::format("--fleet-seed={}", shape.seed));
        formatted = Formatted(std::move(formatted), "FASTCACHE_FLEET_SEED", shape.seed);
        formatted = Formatted(std::move(formatted),
                              "FastCacheFleetIdArgument",
                              shape.fleetId.empty() ? std::string {} : std::format("--fleet-id={}", shape.fleetId));
        formatted = Formatted(std::move(formatted), "FASTCACHE_FLEET_ID", shape.fleetId);
        INFO(formatted);
        // A property this case does not format would reach the parser as text in brackets.
        REQUIRE_FALSE(formatted.contains('['));

        auto const arguments = SplitArguments(formatted);
        REQUIRE(arguments.size() > 1);
        auto const parsed = ParsedFirstStart(std::span { arguments }.subspan(1));
        REQUIRE(parsed.has_value());
        auto const& cfg = Testing::Unwrap(parsed);
        REQUIRE(cfg.installService);
        CHECK(cfg.schedulers.empty());
        CHECK(cfg.advertiseExplicit == !shape.advertised.empty());
        auto const seeds =
            shape.seeded.empty() ? std::vector<std::string> {} : std::vector<std::string> { std::string { shape.seeded } };
        CHECK(cfg.fleetSeeds == seeds);
        CHECK(cfg.firewallAllow
              == (shape.allowed.empty() ? std::vector<std::string> {}
                                        : std::vector<std::string> { std::string { shape.allowed } }));
        auto const rejection = NodeInstallRejection(cfg);
        INFO(rejection.value_or(std::string {}));
        CHECK_FALSE(rejection.has_value());

        auto const spec =
            MakeNodeServiceSpec(std::filesystem::path { "fastcache-compile-node" }, cfg, Testing::InstallerPathProbe());
        CHECK(std::ranges::none_of(spec.arguments, [](std::string const& a) { return a.starts_with("--cluster-dir"); }));
        // The scope is install-time only: it shapes the rules this install creates and is never
        // replayed, so a registration that carried it would re-scope nothing at every start.
        CHECK(std::ranges::none_of(spec.arguments, [](std::string const& a) { return a.starts_with("--firewall-allow"); }));
        auto const advertiseArguments =
            std::ranges::count_if(spec.arguments, [](std::string const& a) { return a.starts_with("--advertise"); });
        CHECK(advertiseArguments == (shape.advertised.empty() ? 0 : 1));
        // The seed is worker state: the registration replays it, one token, as the parser stored it.
        CHECK(std::ranges::count_if(spec.arguments, [](std::string const& a) { return a.starts_with("--fleet-seed"); })
              == (shape.seeded.empty() ? 0 : 1));
        // The pin too, VERBATIM: the registration replays exactly what the operator pasted.
        CHECK(cfg.fleetPin.has_value() == !shape.fleetId.empty());
        if (!shape.fleetId.empty())
            CHECK(std::ranges::contains(spec.arguments, std::format("--fleet-id={}", shape.fleetId)));
        else
            CHECK(std::ranges::none_of(spec.arguments, [](std::string const& a) { return a.starts_with("--fleet-id"); }));
        // The rules the install opens, through the install's own derivation: the reply port is
        // the flag's, so the discovery-reply rule names it and no rule admits any local port.
        Testing::RecordingFirewall firewall;
        NoKeptFormation const fresh;
        auto const installed = InstallWithServiceFirewall(
            [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Created, .message = "installed" }; },
            [] { return ServiceControlResult { .outcome = ServiceControlOutcome::Failed, .message = "not asked" }; },
            cfg,
            // Absolute on every host: a rule whose program is not is refused before the firewall is asked.
            std::filesystem::current_path() / "fastcache-compile-node",
            cfg.serviceName,
            fresh,
            &firewall);
        CHECK(installed.ExitCode() == 0);
        auto const opened = firewall.NamesInGroup(FirewallGroupFor(cfg.serviceName));
        REQUIRE(opened.has_value());
        CHECK(std::ranges::contains(Testing::Unwrap(opened), std::string { shape.replyRule }));
        CHECK(std::ranges::none_of(Testing::Unwrap(opened), [](std::string const& name) { return name.ends_with("/any"); }));

        CHECK(std::ranges::contains(spec.ownedPaths,
                                    OwnedPath { .path = Testing::Unwrap(machineWide),
                                                .privacy = PathPrivacy::Private,
                                                .credentialFiles = { std::filesystem::path { NodeKeyFileName } } }));

        // And the registration comes back up as what was installed.
        auto const reparsed = ParsedFirstStart(spec.arguments);
        REQUIRE(reparsed.has_value());
        // Judged as the START judges it, not only as the install did: what the service runs is the
        // registration, and a rule only the startup table carries would refuse it at every boot.
        auto const startRefusal = StartupPolicyRejection(Testing::Unwrap(reparsed));
        INFO(startRefusal.value_or(std::string {}));
        CHECK_FALSE(startRefusal.has_value());
        CHECK(Testing::Unwrap(reparsed).schedulers.empty()); // a serving node is refused one
        CHECK(Testing::Unwrap(reparsed).advertiseExplicit == !shape.advertised.empty());
        // The pinned reply port is worker state: the registration replays it at every start.
        CHECK(std::to_string(Testing::Unwrap(reparsed).discoveryReplyPort) == shape.replyPort);
        if (!shape.advertised.empty())
            CHECK(Testing::Unwrap(reparsed).advertise == shape.advertised);
        CHECK(Testing::Unwrap(reparsed).fleetSeeds == seeds);
    }
}

namespace
{

/// Every element of @p text opening with @p opener, up to its @p terminator, entities left as written.
/// @param text The fragment.
/// @param opener How the element starts, e.g. `<SetProperty `.
/// @param terminator Where it ends, e.g. `/>`.
/// @return The elements, in document order.
[[nodiscard]] std::vector<std::string> MsiElements(std::string_view text,
                                                   std::string_view opener,
                                                   std::string_view terminator)
{
    auto found = std::vector<std::string> {};
    auto at = text.find(opener);
    while (at != std::string_view::npos)
    {
        auto const end = text.find(terminator, at);
        REQUIRE(end != std::string_view::npos);
        found.emplace_back(text.substr(at, end + terminator.size() - at));
        at = text.find(opener, end);
    }
    return found;
}

/// Every `<Property>` element of @p text, a self-closing one ending at its own `/>` rather than at the
/// next property's `</Property>`.
/// @param text The fragment.
/// @return The elements, in document order.
[[nodiscard]] std::vector<std::string> MsiPropertyElements(std::string_view text)
{
    constexpr std::string_view Opener = "<Property Id=";
    auto found = std::vector<std::string> {};
    auto at = text.find(Opener);
    while (at != std::string_view::npos)
    {
        auto const close = text.find('>', at);
        REQUIRE(close != std::string_view::npos);
        auto end = close + 1;
        if (text[close - 1] != '/')
        {
            constexpr std::string_view Terminator = "</Property>";
            auto const terminator = text.find(Terminator, close);
            REQUIRE(terminator != std::string_view::npos);
            end = terminator + Terminator.size();
        }
        found.emplace_back(text.substr(at, end - at));
        at = text.find(Opener, end);
    }
    return found;
}

/// @param element An element's text.
/// @param name An attribute.
/// @return Its value, or empty when the element has none.
[[nodiscard]] std::string MsiAttribute(std::string_view element, std::string_view name)
{
    auto const key = std::format(" {}=\"", name);
    auto const at = element.find(key);
    if (at == std::string_view::npos)
        return {};
    auto const start = at + key.size();
    auto const end = element.find('"', start);
    REQUIRE(end != std::string_view::npos);
    return std::string { element.substr(start, end - start) };
}

/// The property a `[NAME]` template names, or empty when the value is not exactly one placeholder.
/// @param value An attribute's value.
/// @return `NAME`, or empty.
[[nodiscard]] std::string PlaceholderOf(std::string_view value)
{
    if (value.size() < 3 || !value.starts_with('[') || !value.ends_with(']')
        || value.substr(1, value.size() - 2).contains('['))
        return {};
    return std::string { value.substr(1, value.size() - 2) };
}

/// One step of a transaction the fragment schedules around AppSearch: `property = [source]` when
/// `condition` (one property, truth-tested) is set.
struct MsiCopy
{
    std::string property;  ///< What is set.
    std::string source;    ///< The property whose value it takes.
    std::string condition; ///< The property whose truth gates it.
};

/// What a RegistrySearch reads into a property, or a RegistryValue writes out of one.
struct MsiRemembered
{
    std::string property; ///< The property.
    std::string key;      ///< `Root\Key`.
    std::string name;     ///< The value's name.
};

/// The remember-property rows of `service-actions.xml`, read as Windows Installer reads them.
struct MsiRememberRows
{
    std::vector<MsiCopy> beforeSearch;   ///< `SetProperty Before="AppSearch"`, in document order.
    std::vector<MsiRemembered> searches; ///< Each `Property`'s `RegistrySearch`.
    std::vector<MsiCopy> afterSearch;    ///< `SetProperty After="AppSearch"`, in document order.
    std::vector<MsiRemembered> writes;   ///< What each scheduled `FastCacheRemember` action writes.
};

/// @param text The fragment.
/// @return Its remember-property rows.
[[nodiscard]] MsiRememberRows ReadMsiRememberRows(std::string_view text)
{
    auto rows = MsiRememberRows {};
    for (auto const& element: MsiElements(text, "<SetProperty ", "/>"))
    {
        auto copy = MsiCopy { .property = MsiAttribute(element, "Id"),
                              .source = PlaceholderOf(MsiAttribute(element, "Value")),
                              .condition = MsiAttribute(element, "Condition") };
        if (copy.source.empty())
            continue;
        if (MsiAttribute(element, "Before") == "AppSearch")
            rows.beforeSearch.push_back(std::move(copy));
        else if (MsiAttribute(element, "After") == "AppSearch")
            rows.afterSearch.push_back(std::move(copy));
    }
    for (auto const& property: MsiPropertyElements(text))
        for (auto const& search: MsiElements(property, "<RegistrySearch ", "/>"))
            rows.searches.push_back(MsiRemembered { .property = MsiAttribute(property, "Id"),
                                                    .key = MsiAttribute(search, "Root") + "\\" + MsiAttribute(search, "Key"),
                                                    .name = MsiAttribute(search, "Name") });
    // `reg.exe add <key> /v <name> /t REG_SZ /d "[PROPERTY]"`, one action per value -- counted only
    // when the execute sequence schedules it, since an action nothing runs writes nothing.
    for (auto const& action: MsiElements(text, R"(<CustomAction Id="FastCacheRemember)", "/>"))
    {
        auto const id = MsiAttribute(action, "Id");
        if (!text.contains(std::format(R"(<Custom Action="{}")", id)))
            continue;
        auto command = MsiAttribute(action, "ExeCommand");
        constexpr std::string_view Quote = "&quot;";
        auto quote = command.find(Quote);
        while (quote != std::string::npos)
        {
            command.replace(quote, Quote.size(), "\"");
            quote = command.find(Quote, quote + 1);
        }
        auto const after = [&command](std::string_view marker) {
            auto const at = command.find(marker);
            REQUIRE(at != std::string::npos);
            auto const start = at + marker.size();
            return std::string { command.substr(start, command.find(' ', start) - start) };
        };
        auto value = after(" /d ");
        REQUIRE(value.size() > 2);
        rows.writes.push_back(
            MsiRemembered { .property = PlaceholderOf(std::string_view { value }.substr(1, value.size() - 2)),
                            .key = after(" add "),
                            .name = after(" /v ") });
    }
    return rows;
}

/// The registry, as far as the remembered values go: `(key, name)` to value.
using MsiRegistry = std::map<std::pair<std::string, std::string>, std::string>;

/// One transaction over @p rows: the properties it states, the saves, AppSearch over @p registry,
/// the restores, then the remembered values written back -- in the order the fragment schedules them.
/// @param rows The fragment's remember-property rows.
/// @param stated What the transaction's command line sets.
/// @param registry What the machine holds; updated as the component writes it.
/// @return Every property the transaction ends with.
[[nodiscard]] std::map<std::string, std::string> MsiTransact(MsiRememberRows const& rows,
                                                             std::map<std::string, std::string> stated,
                                                             MsiRegistry& registry)
{
    auto properties = std::move(stated);
    auto const truthy = [&properties](std::string const& name) {
        auto const at = properties.find(name);
        return at != properties.end() && !at->second.empty();
    };
    auto const copy = [&](std::vector<MsiCopy> const& steps) {
        for (auto const& step: steps)
            if (truthy(step.condition))
                properties[step.property] = properties[step.source];
    };
    copy(rows.beforeSearch);
    // AppSearch sets a property only where its search FINDS a value; elsewhere it leaves it alone.
    for (auto const& search: rows.searches)
        if (auto const at = registry.find({ search.key, search.name }); at != registry.end() && !at->second.empty())
            properties[search.property] = at->second;
    copy(rows.afterSearch);
    for (auto const& write: rows.writes)
        registry[{ write.key, write.name }] = properties[write.property];
    return properties;
}

/// The command an `ExeCommand` of the fragment runs, formatted over @p properties, with the
/// fragment's own argument rows (`SetFastCache...Argument`) derived first.
/// @param text The fragment.
/// @param action The custom action's Id.
/// @param properties What the transaction ended with.
/// @return The command.
[[nodiscard]] std::string MsiFormattedCommand(std::string_view text,
                                              std::string_view action,
                                              std::map<std::string, std::string> properties)
{
    // A property the transaction did not set takes the default its `Property` row declares.
    for (auto const& element: MsiPropertyElements(text))
        if (auto value = MsiAttribute(element, "Value"); !value.empty())
            properties.try_emplace(MsiAttribute(element, "Id"), std::move(value));
    for (auto const& element: MsiElements(text, "<SetProperty ", "/>"))
    {
        auto const id = MsiAttribute(element, "Id");
        if (!id.ends_with("Argument"))
            continue;
        auto const condition = MsiAttribute(element, "Condition");
        if (properties[condition].empty())
            continue;
        auto value = MsiAttribute(element, "Value");
        for (auto const& [name, held]: properties)
            value = Formatted(std::move(value), name, held);
        properties[id] = value;
    }
    auto const opener = std::format(R"(<CustomAction Id="{}")", action);
    auto const elements = MsiElements(text, opener, "/>");
    REQUIRE(elements.size() == 1);
    auto command = MsiAttribute(elements.front(), "ExeCommand");
    constexpr std::string_view Quote = "&quot;";
    auto at = command.find(Quote);
    while (at != std::string::npos)
    {
        command.replace(at, Quote.size(), "\"");
        at = command.find(Quote, at + 1);
    }
    command = Formatted(std::move(command), "INSTALL_ROOT", R"(C:\Program Files\fastcached\)");
    // Every property the transaction knows, the derived arguments included; one it never set is empty.
    for (auto const& [name, held]: properties)
        command = Formatted(std::move(command), name, held);
    // A derived argument whose condition did not hold is empty: each one the fragment declares.
    for (auto const& element: MsiElements(text, "<SetProperty ", "/>"))
        if (auto const id = MsiAttribute(element, "Id"); id.ends_with("Argument"))
            command = Formatted(std::move(command), id, "");
    return command;
}

} // namespace

namespace
{
/// Two fleet pins as `fastcache-cli node` prints them: a cluster id and one voter's key each (the
/// RFC 8032 test-vector keys, which are points).
constexpr std::string_view FirstPin = "0123456789abcdef0123456789abcdef@11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo";
constexpr std::string_view SecondPin = "fedcba9876543210fedcba9876543210@_FHNjmIYoaONpH7QAjDwWAgW7RO6MwOsXeuRFUiQgCU";

/// @param cfg A parsed configuration.
/// @return Its fleet pin as `--fleet-id` spells it, or empty when it has none.
[[nodiscard]] std::string PinTextOf(NodeConfig const& cfg)
{
    return cfg.fleetPin.has_value() ? Cluster::FormatPinnedFleet(*cfg.fleetPin) : std::string {};
}
} // namespace

TEST_CASE("An MSI fleet pin the node's parser refuses fails the transaction before it is remembered or registered",
          "[node][docs][service][msi]")
{
    // A pin is security material: an operator who typed one meant the node to join that fleet and no
    // other. The registration is `Return="ignore"`, so its own parse refusing the pin left the OLD
    // registration -- unpinned, or under the old pin -- to be started, with the refused pin remembered
    // and replayed by every later transaction (batch 3 review, B3-1). So the node's arguments are
    // CHECKED first, by `FastCacheNodeCheckArguments` (`Return="check"`, sequenced before every
    // remember write; `check-wix-service-table` pins that), running the node's own parser over them.
    // Here: the check's command, formatted as Windows Installer formats it, is refused -- WHICH
    // refusal, the flag's -- and a value it accepts parses as the check and nothing else.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const text = MsiFragmentText(root);
    auto const checkOf = [&](std::map<std::string, std::string> const& properties) {
        auto const command = MsiFormattedCommand(text, "FastCacheNodeCheckArguments", properties);
        INFO(command);
        REQUIRE_FALSE(command.contains('['));
        auto const arguments = SplitArguments(command);
        REQUIRE(arguments.size() > 1);
        return ParsedFirstStart(std::span { arguments }.subspan(1));
    };

    // What is checked is what would be registered: the same arguments after each verb, for a
    // transaction stating every property.
    auto const every = std::map<std::string, std::string> { { "FASTCACHE_FIREWALL_ALLOW", "10.0.0.0/8" },
                                                            { "FASTCACHE_NODE_ADVERTISE", "worker-01.internal:6674" },
                                                            { "FASTCACHE_FLEET_SEED", "office-a.vpn.example" },
                                                            { "FASTCACHE_FLEET_ID", std::string { FirstPin } } };
    auto const after = [](std::string const& command, std::string_view verb) {
        auto const at = command.find(verb);
        REQUIRE(at != std::string::npos);
        return command.substr(at + verb.size());
    };
    CHECK(after(MsiFormattedCommand(text, "FastCacheNodeCheckArguments", every), "--check-arguments")
          == after(MsiFormattedCommand(text, "FastCacheNodeInstallService", every), "--install-service"));

    struct Refused
    {
        std::string_view property; ///< The MSI property the transaction states.
        std::string_view value;    ///< A value the node's parser refuses.
        std::string_view flag;     ///< The flag the refusal must name.
    };
    for (auto const& row: std::to_array<Refused>({
             { .property = "FASTCACHE_FLEET_ID", .value = "0123456789abcdef0123456789abcdef", .flag = "--fleet-id" },
             { .property = "FASTCACHE_FLEET_ID", .value = "office@not-a-key", .flag = "--fleet-id" },
             // The seed rides the same check: the step-20 re-check's N-3 shape, where a malformed one
             // registered nothing and said so to nobody.
             { .property = "FASTCACHE_FLEET_SEED", .value = "office-a.vpn.example:99999", .flag = "--fleet-seed" },
         }))
    {
        INFO(row.property << "=" << row.value);
        auto const parsed = checkOf({ { std::string { row.property }, std::string { row.value } } });
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().ToString().contains(row.flag));
    }

    // The control: a pin the parser reads passes the check, and the check is all it asks for.
    auto const accepted = checkOf(every);
    REQUIRE(accepted.has_value());
    CHECK(Testing::Unwrap(accepted).checkArguments);
    CHECK_FALSE(Testing::Unwrap(accepted).installService);
    CHECK(PinTextOf(Testing::Unwrap(accepted)) == FirstPin);
}

TEST_CASE(
    "The MSI remembers its optional properties: a repair or upgrade that leaves them out re-registers what was installed",
    "[node][docs][service][msi]")
{
    // Every transaction that leaves a feature installed re-registers its service, so a property the
    // MSI did not remember would be dropped by the next repair: the firewall scope widened to any
    // address, which fails OPEN, and the advertised endpoint lost. The fragment's own rows are read
    // and run as Windows Installer runs them, across install, repair, upgrade and repair again.
    std::filesystem::path const root { FASTCACHED_SOURCE_DIR };
    auto const text = MsiFragmentText(root);
    auto const rows = ReadMsiRememberRows(text);
    INFO(std::format("{} save(s), {} search(es), {} restore(s), {} write(s)",
                     rows.beforeSearch.size(),
                     rows.searches.size(),
                     rows.afterSearch.size(),
                     rows.writes.size()));
    // The premise, so an emptied fragment cannot pass by remembering nothing at all -- and the key the
    // actions write is the key the searches read, or nothing written is ever read back.
    for (std::string_view const property:
         { "FASTCACHE_FIREWALL_ALLOW", "FASTCACHE_NODE_ADVERTISE", "FASTCACHE_FLEET_SEED", "FASTCACHE_FLEET_ID" })
    {
        INFO(property);
        CHECK(std::ranges::any_of(rows.searches, [&](MsiRemembered const& row) { return row.property == property; }));
        CHECK(std::ranges::any_of(rows.writes, [&](MsiRemembered const& row) { return row.property == property; }));
    }

    auto registry = MsiRegistry {};
    auto const node = [&](std::map<std::string, std::string> const& properties) {
        return MsiFormattedCommand(text, "FastCacheNodeInstallService", properties);
    };
    auto const daemon = [&](std::map<std::string, std::string> const& properties) {
        return MsiFormattedCommand(text, "FastCachedInstallService", properties);
    };
    auto const parsed = [](std::string const& command) {
        INFO(command);
        REQUIRE_FALSE(command.contains('['));
        auto const arguments = SplitArguments(command);
        REQUIRE(arguments.size() > 1);
        auto cfg = ParsedFirstStart(std::span { arguments }.subspan(1));
        REQUIRE(cfg.has_value());
        return Testing::Unwrap(cfg);
    };

    {
        INFO("the install states a scope, an advertised endpoint, a fleet seed and a fleet pin");
        auto const installed = MsiTransact(rows,
                                           { { "FASTCACHE_FIREWALL_ALLOW", "10.0.0.0/8" },
                                             { "FASTCACHE_NODE_ADVERTISE", "worker-01.internal:6674" },
                                             { "FASTCACHE_FLEET_SEED", "office-a.vpn.example" },
                                             { "FASTCACHE_FLEET_ID", std::string { FirstPin } } },
                                           registry);
        auto const cfg = parsed(node(installed));
        CHECK(cfg.firewallAllow == std::vector<std::string> { "10.0.0.0/8" });
        CHECK(cfg.advertise == "worker-01.internal:6674");
        CHECK(cfg.fleetSeeds == std::vector<std::string> { "office-a.vpn.example:6674" });
        CHECK(PinTextOf(cfg) == FirstPin);
    }
    {
        INFO("a repair states nothing, and registers all four again");
        auto const repaired = MsiTransact(rows, {}, registry);
        auto const cfg = parsed(node(repaired));
        CHECK(cfg.firewallAllow == std::vector<std::string> { "10.0.0.0/8" });
        CHECK(cfg.advertiseExplicit);
        CHECK(cfg.advertise == "worker-01.internal:6674");
        CHECK(cfg.fleetSeeds == std::vector<std::string> { "office-a.vpn.example:6674" });
        CHECK(PinTextOf(cfg) == FirstPin);
        // fastcached's registration takes the same scope from the same remembered value.
        CHECK(daemon(repaired).contains("--firewall-allow=10.0.0.0/8"));
    }
    {
        INFO("an upgrade that states a NEW scope, seed and pin gets them, and keeps the remembered endpoint");
        auto const upgraded = MsiTransact(rows,
                                          { { "FASTCACHE_FIREWALL_ALLOW", "192.168.0.0/16" },
                                            { "FASTCACHE_FLEET_SEED", "office-b.vpn.example" },
                                            { "FASTCACHE_FLEET_ID", std::string { SecondPin } } },
                                          registry);
        auto const cfg = parsed(node(upgraded));
        CHECK(cfg.firewallAllow == std::vector<std::string> { "192.168.0.0/16" });
        CHECK(cfg.advertise == "worker-01.internal:6674");
        CHECK(cfg.fleetSeeds == std::vector<std::string> { "office-b.vpn.example:6674" });
        CHECK(PinTextOf(cfg) == SecondPin);
    }
    {
        INFO("and the next repair registers the new scope, seed and pin, not the first ones");
        auto const repaired = MsiTransact(rows, {}, registry);
        auto const cfg = parsed(node(repaired));
        CHECK(cfg.firewallAllow == std::vector<std::string> { "192.168.0.0/16" });
        CHECK(cfg.fleetSeeds == std::vector<std::string> { "office-b.vpn.example:6674" });
        CHECK(PinTextOf(cfg) == SecondPin);
    }
    {
        // The control: a machine that never had a scope remembers none, so a repair adds nothing.
        INFO("a machine installed with no property repairs with none");
        auto fresh = MsiRegistry {};
        (void) MsiTransact(rows, {}, fresh);
        auto const cfg = parsed(node(MsiTransact(rows, {}, fresh)));
        CHECK(cfg.firewallAllow.empty());
        CHECK_FALSE(cfg.advertiseExplicit);
        CHECK(cfg.fleetSeeds.empty());
        CHECK_FALSE(cfg.fleetPin.has_value());
    }
}
