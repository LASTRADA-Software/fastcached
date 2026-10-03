// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/ProcessExit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/Ranges.hpp>

using FastCache::ProcessExit;
using FastCache::StartStage;

namespace
{

/// One step of a start, and the exit it must give up with.
struct StageVerdict
{
    StartStage stage;     ///< The step.
    ProcessExit exit;     ///< How the process must end when it gives up.
    std::string_view why; ///< Why a retry does or does not change the outcome.
};

/// Every step, written out here rather than read back from the table, so a row whose exit is
/// changed -- or a step added without a decision -- fails by name.
constexpr auto StageVerdicts = std::to_array<StageVerdict>({
    { .stage = StartStage::CommandLine, .exit = ProcessExit::Refused, .why = "the same argv parses the same way" },
    { .stage = StartStage::ConfigurationFile,
      .exit = ProcessExit::Refused,
      .why = "bytes that were read fail to load, or to assemble, the same way again" },
    { .stage = StartStage::ConfigurationFileIo,
      .exit = ProcessExit::Failed,
      .why = "a named file absent or unreadable at boot may be there once a share or a mount is" },
    { .stage = StartStage::NoOutcome, .exit = ProcessExit::Refused, .why = "the same argv selects nothing again" },
    { .stage = StartStage::StartupRules,
      .exit = ProcessExit::Refused,
      .why = "a startup rule is a function of the configuration alone" },
    { .stage = StartStage::IdentityKey,
      .exit = ProcessExit::Refused,
      .why = "a key file read and found unusable is read the same, and never re-minted" },
    { .stage = StartStage::IdentityKeyIo,
      .exit = ProcessExit::Failed,
      .why = "an open, a write or a draw that failed may succeed at the next start" },
    { .stage = StartStage::NodeIdentity,
      .exit = ProcessExit::Refused,
      .why = "an identity file read and found empty or not text is read the same" },
    { .stage = StartStage::NodeIdentityIo,
      .exit = ProcessExit::Failed,
      .why = "a read, a directory, a write or a draw that failed may succeed at the next start" },
    { .stage = StartStage::Formation,
      .exit = ProcessExit::Failed,
      .why = "its arms are of both kinds -- a read or a save that failed, and bytes that do not decode -- so a "
             "refusal is restarted a bounded number of times rather than a failure left stopped" },
    { .stage = StartStage::TlsMaterial, .exit = ProcessExit::Refused, .why = "nothing names the material" },
    { .stage = StartStage::TlsUnavailable, .exit = ProcessExit::Refused, .why = "the build does not change" },
    { .stage = StartStage::KeyspaceEvents, .exit = ProcessExit::Refused, .why = "the grammar does not change" },
    { .stage = StartStage::TlsLoad,
      .exit = ProcessExit::Failed,
      .why = "a certificate is replaced in place, and read mid-rotation" },
    { .stage = StartStage::Storage,
      .exit = ProcessExit::Failed,
      .why = "a store held by a process that is exiting is free at the next start" },
});

/// The whole of a source file.
/// @param relative The path under the source tree.
/// @return Its bytes; the case has failed when it cannot be read.
[[nodiscard]] std::string SourceText(std::filesystem::path const& relative)
{
    auto const path = std::filesystem::path { FASTCACHED_SOURCE_DIR } / relative;
    REQUIRE(std::filesystem::exists(path));
    // Through the stream buffer, for `PosixDaemonHost_test`'s measured reason.
    std::ifstream in { path, std::ios::binary };
    REQUIRE(in);
    std::ostringstream contents;
    contents << in.rdbuf();
    return std::move(contents).str();
}

/// How often @p name occurs in @p text as a whole name: `StartStage::IdentityKey` is not counted
/// inside `StartStage::IdentityKeyIo`.
/// @param text Where to look.
/// @param name What to count; never empty.
/// @return The number of occurrences not followed by another identifier character.
[[nodiscard]] std::size_t NameOccurrences(std::string_view text, std::string_view name)
{
    auto count = std::size_t { 0 };
    auto at = text.find(name);
    while (at != std::string_view::npos)
    {
        auto const next = at + name.size();
        auto const continues =
            next < text.size() && (std::isalnum(static_cast<unsigned char>(text[next])) != 0 || text[next] == '_');
        if (!continues)
            ++count;
        at = text.find(name, next);
    }
    return count;
}

/// How often @p needle occurs in @p text.
/// @param text Where to look.
/// @param needle What to count; never empty.
/// @return The number of non-overlapping occurrences.
[[nodiscard]] std::size_t Occurrences(std::string_view text, std::string_view needle)
{
    auto count = std::size_t { 0 };
    auto at = text.find(needle);
    while (at != std::string_view::npos)
    {
        ++count;
        at = text.find(needle, at + needle.size());
    }
    return count;
}

/// A step's enumerator as a daemon's source spells it.
/// @param stage The step.
/// @return `StartStage::<Name>`.
[[nodiscard]] std::string Spelled(StartStage stage)
{
    constexpr auto names = std::to_array<std::pair<StartStage, std::string_view>>({
        { StartStage::CommandLine, "CommandLine" },
        { StartStage::ConfigurationFile, "ConfigurationFile" },
        { StartStage::ConfigurationFileIo, "ConfigurationFileIo" },
        { StartStage::NoOutcome, "NoOutcome" },
        { StartStage::StartupRules, "StartupRules" },
        { StartStage::IdentityKey, "IdentityKey" },
        { StartStage::IdentityKeyIo, "IdentityKeyIo" },
        { StartStage::NodeIdentity, "NodeIdentity" },
        { StartStage::NodeIdentityIo, "NodeIdentityIo" },
        { StartStage::Formation, "Formation" },
        { StartStage::TlsMaterial, "TlsMaterial" },
        { StartStage::TlsUnavailable, "TlsUnavailable" },
        { StartStage::KeyspaceEvents, "KeyspaceEvents" },
        { StartStage::TlsLoad, "TlsLoad" },
        { StartStage::Storage, "Storage" },
    });
    // `core::findOrNull` rather than an iterator: `std::array`'s is a raw pointer on one standard
    // library, and the qualifier clang-tidy then asks for does not compile on the other.
    auto const* const found = core::findOrNull(names, stage, &std::pair<StartStage, std::string_view>::first);
    REQUIRE(found != nullptr);
    return std::format("StartStage::{}", found->second);
}

} // namespace

TEST_CASE("A refused start and a failed one leave the process as different codes", "[platform][startup][exit]")
{
    CHECK(FastCache::ExitCodeOf(ProcessExit::Served) == EXIT_SUCCESS);
    CHECK(FastCache::ExitCodeOf(ProcessExit::Failed) == EXIT_FAILURE);
    // EX_CONFIG: systemd names it `78/CONFIG`, and nothing else either daemon returns is 78.
    CHECK(FastCache::ExitCodeOf(ProcessExit::Refused) == 78);
    // A one-shot command's decline: the usage exit.
    CHECK(FastCache::ExitCodeOf(ProcessExit::Usage) == 2);

    // A supervisor that can tell them apart restarts a failure and nothing else.
    for (auto const& row: FastCache::ProcessExitRows)
    {
        INFO(row.meaning);
        CHECK(row.restarted == (row.exit == ProcessExit::Failed));
        CHECK(std::ranges::count(FastCache::ProcessExitRows, row.code, &FastCache::ProcessExitRow::code) == 1);
    }
}

TEST_CASE("A one-shot command's ending is a decision or transient, and one table codes it", "[platform][exit]")
{
    struct EndingVerdict
    {
        FastCache::CommandEnding ending;
        int code;
        std::string_view why;
    };
    constexpr auto verdicts = std::to_array<EndingVerdict>({
        { .ending = FastCache::CommandEnding::Completed, .code = 0, .why = "it did what it was asked" },
        { .ending = FastCache::CommandEnding::Declined, .code = 2, .why = "a decision: do not retry" },
        { .ending = FastCache::CommandEnding::Failed, .code = 1, .why = "transient or part-way: a retry may help" },
    });
    for (auto const ending: FastCache::Enumerators<FastCache::CommandEnding>())
        CHECK(std::ranges::count(verdicts, ending, &EndingVerdict::ending) == 1);
    for (auto const& verdict: verdicts)
    {
        INFO(verdict.why);
        CHECK(FastCache::CommandExitCode(verdict.ending) == verdict.code);
    }
    CHECK(FastCache::ExitCodeOf(ProcessExit::Usage) == FastCache::CommandExitCode(FastCache::CommandEnding::Declined));

    // The one fact that decides an unfinished command: whether a retry may get past it.
    CHECK(FastCache::UnfinishedEnding(false) == FastCache::CommandEnding::Declined);
    CHECK(FastCache::UnfinishedEnding(true) == FastCache::CommandEnding::Failed);

    // In order of severity, which a command over several parts reads with `std::max`: a run with
    // one shard stopped part-way and one refused has something a re-run finishes, and says so.
    CHECK(FastCache::CommandEnding::Completed < FastCache::CommandEnding::Declined);
    CHECK(FastCache::CommandEnding::Declined < FastCache::CommandEnding::Failed);
    CHECK(std::max(FastCache::CommandEnding::Declined, FastCache::CommandEnding::Failed)
          == FastCache::CommandEnding::Failed);
}

TEST_CASE("A one-shot command stopped by a configuration error ends by its arm, as a start does", "[platform][exit][config]")
{
    // Every code written out, so one added later is decided. A file absent, unreadable or not
    // written is an I/O arm, transient (1); a verdict on what was read is a decision (2) -- the
    // start's split (`ConfigurationFileStage`), asked of a command.
    struct CodeEnding
    {
        FastCache::ConfigErrorCode code;
        FastCache::CommandEnding ending;
    };
    constexpr auto verdicts = std::to_array<CodeEnding>({
        { .code = FastCache::ConfigErrorCode::Ok, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::FileNotFound, .ending = FastCache::CommandEnding::Failed },
        { .code = FastCache::ConfigErrorCode::ParseError, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::UnknownKey, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::TypeMismatch, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::OutOfRange, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::MissingRequired, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::ImmutableChanged, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::UndefinedVariable, .ending = FastCache::CommandEnding::Declined },
        { .code = FastCache::ConfigErrorCode::WriteFailed, .ending = FastCache::CommandEnding::Failed },
        { .code = FastCache::ConfigErrorCode::FileUnreadable, .ending = FastCache::CommandEnding::Failed },
    });
    CHECK(static_cast<std::size_t>(FastCache::ConfigErrorCode::FileUnreadable) + 1 == verdicts.size());
    for (auto const& verdict: verdicts)
    {
        INFO(FastCache::ToStringView(verdict.code));
        CHECK(FastCache::EndingOf(verdict.code) == verdict.ending);
    }
}

TEST_CASE("A command stopped at a step ends as a start there would: an I/O arm transient, a verdict a decision",
          "[platform][startup][exit]")
{
    for (auto const stage: FastCache::Enumerators<StartStage>())
    {
        INFO(Spelled(stage));
        CHECK(FastCache::EndingOf(stage)
              == (FastCache::ExitOf(stage) == ProcessExit::Failed ? FastCache::CommandEnding::Failed
                                                                  : FastCache::CommandEnding::Declined));
    }
    CHECK(FastCache::EndingOf(StartStage::IdentityKeyIo) == FastCache::CommandEnding::Failed);
    CHECK(FastCache::EndingOf(StartStage::IdentityKey) == FastCache::CommandEnding::Declined);
}

TEST_CASE("A refusal before an outcome runs answers what that outcome's reader reads", "[platform][startup][exit]")
{
    // A supervisor reads a start's own code; an operator reads the ending a one-shot command stopped
    // at that step has -- an I/O arm 1 and a verdict 2, the stage's own arm; a health probe's
    // reader knows 0 and 1 only, so every refusal is 1.
    for (auto const reader: FastCache::Enumerators<FastCache::ExitReader>())
        for (auto const stage: FastCache::Enumerators<StartStage>())
        {
            INFO(Spelled(stage));
            auto const code = FastCache::RefusalExitCode(reader, stage);
            switch (reader)
            {
                case FastCache::ExitReader::Supervisor:
                    CHECK(code == FastCache::ExitCodeFor(stage));
                    break;
                case FastCache::ExitReader::Operator:
                    CHECK(code == FastCache::CommandExitCode(FastCache::EndingOf(stage)));
                    break;
                case FastCache::ExitReader::Prober:
                    CHECK(code == 1);
                    break;
                case FastCache::ExitReader::Last:
                    FAIL("Last is no reader");
                    break;
            }
        }
    // Written out, so a row that stops deriving fails by name: a named file that is absent is
    // transient to the operator as to a supervisor, and one that did not parse is a decision.
    CHECK(FastCache::RefusalExitCode(FastCache::ExitReader::Operator, StartStage::ConfigurationFileIo) == 1);
    CHECK(FastCache::RefusalExitCode(FastCache::ExitReader::Operator, StartStage::ConfigurationFile) == 2);
    CHECK(FastCache::RefusalExitCode(FastCache::ExitReader::Operator, StartStage::CommandLine) == 2);
}

TEST_CASE("No step of a start gives up with a one-shot command's exit", "[platform][startup][exit]")
{
    // A START is what a supervisor reads, so it answers 1 or 78 and never the usage exit, which is
    // a one-shot command's answer to an operator at a terminal -- whatever refused it. The column
    // says so, written out here so a row that changes its mind fails by name.
    CHECK_FALSE(FastCache::ProcessExitRows[static_cast<std::size_t>(ProcessExit::Usage)].endsAStart);
    for (auto const exit: { ProcessExit::Served, ProcessExit::Failed, ProcessExit::Refused })
        CHECK(FastCache::ProcessExitRows[static_cast<std::size_t>(exit)].endsAStart);
    for (auto const stage: FastCache::Enumerators<StartStage>())
    {
        INFO(Spelled(stage));
        CHECK(FastCache::ProcessExitRows[static_cast<std::size_t>(FastCache::ExitOf(stage))].endsAStart);
    }
}

TEST_CASE("Every step of a start gives up as a refusal only when a retry reaches the same verdict",
          "[platform][startup][exit]")
{
    for (auto const stage: FastCache::Enumerators<StartStage>())
        CHECK(std::ranges::count(StageVerdicts, stage, &StageVerdict::stage) == 1);

    for (auto const& verdict: StageVerdicts)
    {
        INFO(Spelled(verdict.stage) << ": " << verdict.why);
        CHECK(FastCache::ExitOf(verdict.stage) == verdict.exit);
        CHECK(FastCache::ExitCodeFor(verdict.stage) == FastCache::ExitCodeOf(verdict.exit));
    }
}

TEST_CASE("A configuration error is the refusal only when it is a verdict on bytes that were read",
          "[platform][startup][exit]")
{
    // Every verdict on bytes that were read is the refusal. Every I/O arm is the failure: a named file
    // that is absent (a share or a mount a boot-time start raced), one that is there and could not be
    // read, and a write. Every code written out, so one added later is decided.
    struct CodeVerdict
    {
        FastCache::ConfigErrorCode code;
        StartStage stage;
    };
    constexpr auto verdicts = std::to_array<CodeVerdict>({
        { .code = FastCache::ConfigErrorCode::Ok, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::FileNotFound, .stage = StartStage::ConfigurationFileIo },
        { .code = FastCache::ConfigErrorCode::ParseError, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::UnknownKey, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::TypeMismatch, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::OutOfRange, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::MissingRequired, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::ImmutableChanged, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::UndefinedVariable, .stage = StartStage::ConfigurationFile },
        { .code = FastCache::ConfigErrorCode::WriteFailed, .stage = StartStage::ConfigurationFileIo },
        { .code = FastCache::ConfigErrorCode::FileUnreadable, .stage = StartStage::ConfigurationFileIo },
    });
    // The codes are consecutive from `Ok`, and `FileUnreadable` is the last: a code added after it
    // lands outside this list, which the count below catches.
    CHECK(static_cast<std::size_t>(FastCache::ConfigErrorCode::FileUnreadable) + 1 == verdicts.size());
    for (auto const& verdict: verdicts)
    {
        INFO(FastCache::ToStringView(verdict.code));
        CHECK(FastCache::ConfigurationFileStage(verdict.code) == verdict.stage);
    }
}

TEST_CASE("Both daemons give up at each step through the table, never with a bare code", "[platform][startup][exit]")
{
    // The mains are in no test target, so what holds them to the table is their text: every step
    // each one gives up at, named as often as it has sites, and no refusal with a code of its own.
    struct Site
    {
        std::filesystem::path main; ///< The daemon's `main.cpp`, under the source tree.
        std::array<std::size_t, FastCache::EnumeratorCount<StartStage>> sites; ///< Per step, in enumerator order.
    };
    auto const daemons = std::to_array<Site>({
        // CommandLine, ConfigurationFile, ConfigurationFileIo, NoOutcome, StartupRules, IdentityKey,
        // IdentityKeyIo, NodeIdentity, NodeIdentityIo, Formation, TlsMaterial, TlsUnavailable,
        // KeyspaceEvents, TlsLoad, Storage. The steps with an I/O arm are chosen BY the arm, through
        // `ConfigurationFileStage` and `StageOf(fault)`, so neither main names them -- but for the
        // node's pre-verb state-directory refusal, which is I/O whichever way it went
        // (`NodeIdentityIo`). The node's serving body gives up through its own table,
        // `NodeRefusalCauses` (`NodeRefusal_test`).
        { .main = std::filesystem::path { "src" } / "apps" / "fastcached" / "main.cpp",
          .sites = { 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1 } },
        { .main = std::filesystem::path { "src" } / "apps" / "fastcache-compile-node" / "main.cpp",
          .sites = { 1, 0, 0, 0, 2, 0, 0, 0, 1, 5, 0, 0, 0, 0, 0 } },
    });
    // And the node chooses its key's and identity's step by the fault that refused.
    auto const node = SourceText(std::filesystem::path { "src" } / "apps" / "fastcache-compile-node" / "main.cpp");
    CHECK(Occurrences(node, "ExitCodeFor(StageOf(identityKey.error().fault))") == 1);
    CHECK(Occurrences(node, "ExitCodeFor(StageOf(identity.error().fault))") == 1);
    // A one-shot verb is never supervised, so a refusal decided before it runs answers what the
    // operator's row of `ExitReaderRows` says -- the ROW the daemon's one-shots read too, so one
    // change reaches both binaries: asked once, in the one function both sites go through.
    // `node-config-file-e2e` drives it through the binary; this holds it where that fixture does
    // not run.
    CHECK(Occurrences(node,
                      "if (SelectsOneShotVerb(commandLine))\n        return RefusalExitCode(ExitReader::Operator, stage);")
          == 1);
    // And the daemon's, from `CliOutcomeTable`'s reader column, asked in its one refusal function of
    // the command line's own parse. `exit-codes-e2e` drives it through the binaries.
    auto const daemonMain = SourceText(std::filesystem::path { "src" } / "apps" / "fastcached" / "main.cpp");
    CHECK(Occurrences(daemonMain,
                      "if (auto const reader = FastCache::ExitReaderOf(commandLine.outcome); reader != "
                      "FastCache::ExitReader::Supervisor)\n        return FastCache::RefusalExitCode(reader, stage);")
          == 1);

    for (auto const& daemon: daemons)
    {
        INFO(daemon.main.generic_string());
        auto const text = SourceText(daemon.main);
        for (auto const stage: FastCache::Enumerators<StartStage>())
        {
            INFO(Spelled(stage));
            CHECK(NameOccurrences(text, Spelled(stage)) == daemon.sites.at(static_cast<std::size_t>(stage)));
        }
        // The configuration file's step comes from the error's own arm.
        CHECK(Occurrences(text, "ConfigurationFileStage(") == 1);
        // A refusal reported to the service manager carries the step's code, never one of its own.
        CHECK(Occurrences(text, ", ExitUsage)") == 0);
        CHECK(Occurrences(text, ", EXIT_FAILURE)") == 0);
        CHECK(Occurrences(text, "Refuse(ExitUsage") == 0);
        CHECK(Occurrences(text, "Refuse(EXIT_FAILURE") == 0);
    }
}

namespace
{

/// A line in one of the mains that may spell an exit code, and why.
struct SpelledCodeExemption
{
    std::string_view file;   ///< The main, as `SpellingMains` names it.
    std::string_view line;   ///< The whole line, leading and trailing blanks trimmed.
    std::string_view reason; ///< Why this site must spell a code rather than state an ending.
};

/// Every exempted site. None today: a row carries its reason, and a row that stops matching a line
/// is refused as stale, so an exemption cannot outlive the site it excused.
inline constexpr std::array<SpelledCodeExemption, 0> SpelledCodeExemptions {};
static_assert(std::ranges::none_of(SpelledCodeExemptions,
                                   [](SpelledCodeExemption const& row) { return row.reason.empty(); }),
              "an exemption states why the site must spell a code");

/// The mains the rule holds, under the source tree.
/// @return Both.
[[nodiscard]] std::array<std::filesystem::path, 2> SpellingMains()
{
    return { std::filesystem::path { "src" } / "apps" / "fastcached" / "main.cpp",
             std::filesystem::path { "src" } / "apps" / "fastcache-compile-node" / "main.cpp" };
}

/// Every line of @p text that spells an exit code, whole-line comments excepted, blanks trimmed.
///
/// Four shapes, each a SHAPE rather than a list of values: a `return` of an integer literal
/// (signed, parenthesised, hexadecimal, binary or suffixed); any call of `exit`, `_exit`, `_Exit` or
/// `quick_exit`, qualified or not, whatever its argument; the C library's `EXIT_SUCCESS` and
/// `EXIT_FAILURE`; and the retired local constants `ExitUsage` and `ExitOk`.
///
/// **Blind spot, and its direction: it fails OPEN.** A code reached through a NAME or an
/// EXPRESSION -- a variable, a ternary, a cast, a function other than the one mapping -- is not
/// seen, so `return code;` passes whatever `code` holds. The case below asserts that too, so the
/// gap is on record rather than discovered.
/// @param text A source file.
/// @return The offending lines.
[[nodiscard]] std::vector<std::string> SpelledExitCodes(std::string_view text)
{
    static std::regex const spelled {
        R"(\breturn\s*\(?\s*[-+]?\s*(0[xX][0-9a-fA-F']+|0[bB][01']+|[0-9][0-9']*)[uUlLzZ]*\s*\)?\s*;)"
        R"(|(^|[^A-Za-z0-9_])(_exit|_Exit|exit|quick_exit)\s*\()"
        R"(|\bEXIT_(SUCCESS|FAILURE)\b|\bExit(Usage|Ok)\b)"
    };
    std::vector<std::string> found;
    for (auto const piece: std::views::split(text, '\n'))
    {
        auto line = std::string_view { piece.begin(), piece.end() };
        auto const first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos)
            continue;
        line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
        if (line.starts_with("//"))
            continue;
        auto const owned = std::string { line };
        if (std::regex_search(owned, spelled))
            found.push_back(owned);
    }
    return found;
}

/// What the check says when it refuses: the rule, the remedy and what it does NOT cover.
constexpr std::string_view SpelledCodeRefusal =
    "a main spells an exit code. State the ENDING and return CommandExitCode(ending) -- or, for a "
    "start, ExitCodeFor(stage) / ExitCodeOf(exit) -- so a row change reaches this site. A site that "
    "genuinely must is a SpelledCodeExemptions row with its reason. NOT covered, and failing OPEN: a "
    "code reached through a name or an expression (a variable, a ternary, a cast) passes this check";

} // namespace

TEST_CASE("No site in either main spells an exit code", "[platform][startup][exit]")
{
    // A one-shot command states its ENDING and `CommandExitCode` is the one mapping to a code; a
    // start answers through `ExitCodeFor` or `ExitCodeOf`. So neither main spells a code itself --
    // not a literal of any value, not a direct exit call, not the C library's two, not a constant
    // standing for one -- and a site that decides per site is one a row change does not reach.
    // What the scan cannot see is `SpelledExitCodes`' blind spot, which fails OPEN; see there.

    // The scan sees every shape it claims, one line each, and neither a comment nor the mapping.
    constexpr auto planted = std::to_array<std::string_view>({
        "    return 3;",
        "    return -1;",
        "    return ( 0x2 );",
        "    return 7u;",
        "    std::exit(4);",
        "    ::_exit(code);",
        "    quick_exit(EXIT_FAILURE);",
        "    return EXIT_SUCCESS;",
    });
    for (auto const line: planted)
    {
        INFO(line);
        CHECK(SpelledExitCodes(line).size() == 1);
    }
    CHECK(SpelledExitCodes("    // return 3;").empty());
    CHECK(SpelledExitCodes("    return CommandExitCode(CommandEnding::Declined);").empty());
    CHECK(SpelledExitCodes("    return ExitCodeOf(ProcessExit::Served);").empty());
    CHECK(SpelledExitCodes("    return ExitCodeFor(StartStage::Storage);").empty());
    CHECK(SpelledExitCodes("    auto const onExit = [] {};").empty());
    // The blind spot, asserted as what it is: a code through a name passes.
    CHECK(SpelledExitCodes("    return code;").empty());

    for (auto const& file: SpellingMains())
    {
        INFO(file.generic_string());
        auto const text = SourceText(file);
        // The positive control: the one mapping IS what these files spell, so no finding below is
        // a file this case could not read.
        CHECK(Occurrences(text, "CommandExitCode(") > 0);
        auto const found = SpelledExitCodes(text);
        for (auto const& line: found)
        {
            auto const exempt = std::ranges::any_of(SpelledCodeExemptions, [&](SpelledCodeExemption const& row) {
                return row.file == file.generic_string() && row.line == line;
            });
            if (exempt)
                continue;
            INFO(line);
            FAIL_CHECK(SpelledCodeRefusal);
        }
        // An exemption that no longer matches a line excuses nothing, and is refused as stale.
        for (auto const& row: SpelledCodeExemptions)
            if (row.file == file.generic_string())
            {
                INFO(row.line);
                CHECK(std::ranges::count(found, std::string { row.line }) == 1);
            }
    }
}
