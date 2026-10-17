// SPDX-License-Identifier: Apache-2.0
#include "NodeConditions.hpp"
#include "RefusedArguments.hpp"

#include <FastCache/Core/Logger.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <Dispatch.hpp>
#include <IProcessRunner.hpp>
#include <WorkerProtocol.hpp>
#include <tests/CompileReplyFakes.hpp>
#include <tests/ScratchPath.hpp>

using namespace FastCache;
using namespace FastCache::Node;

namespace
{
namespace Wire = CompileCacheWire;

/// The row this report answers, as the wire carries it.
/// @param conditions The registry.
/// @return The row; every snapshot carries every row, so it is always there.
[[nodiscard]] Wire::NodeConditionFields RefusedRow(NodeConditions const& conditions)
{
    auto const rows = conditions.Snapshot();
    auto const found =
        std::ranges::find(rows, RowFor(NodeCondition::RefusedCompileArguments).id, &Wire::NodeConditionFields::id);
    REQUIRE(found != rows.end());
    return *found;
}

/// How many captured lines mention @p text.
/// @param logger The capture.
/// @param text What to look for.
/// @return The count.
[[nodiscard]] std::size_t LinesMentioning(CapturingLogger const& logger, std::string_view text)
{
    auto const records = logger.Snapshot();
    return static_cast<std::size_t>(std::ranges::count_if(
        records, [text](CapturingLogger::Record const& record) { return record.message.contains(text); }));
}

} // namespace

TEST_CASE("A worker that refused nothing has checked, and says clear", "[node][conditions][refused-arguments]")
{
    // Not undecided: the worker runs, so the question was asked and the answer is "none".
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport const report { conditions, logger, {} };
    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);
    CHECK(logger.Snapshot().empty());
}

TEST_CASE("A refused argument is named in the row and logged once, however often it recurs",
          "[node][conditions][refused-arguments]")
{
    // What the node never said: WHICH argument. The counter rose 372 times over one flag and the
    // flag was nowhere -- so the row names it, and the log says it exactly once, because a build
    // refusing one flag on every unit must not write one line per unit. `/volatile:iso` rather
    // than that story's `-external:W0`, which this build's worker takes -- and the report asks
    // the worker's own rules, so a refusal naming it would be judged admitted and not reported.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };

    for ([[maybe_unused]] auto const i: std::views::iota(0, 3))
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("/volatile:iso", Cc::Flavor::Cl));

    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
    auto const row = RefusedRow(conditions);
    CHECK(row.detail.contains("/volatile:iso"));
    CHECK(row.detail.contains("3 compile(s)"));
    CHECK(row.persistence == "live");
    CHECK(LinesMentioning(logger, "/volatile:iso") == 1);

    // A second argument is a second line and joins the list.
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fanalyzer", Cc::Flavor::Gcc));
    CHECK(LinesMentioning(logger, "-fanalyzer") == 1);
    CHECK(LinesMentioning(logger, "/volatile:iso") == 1);
    auto const both = RefusedRow(conditions);
    CHECK(both.detail.contains("/volatile:iso"));
    CHECK(both.detail.contains("-fanalyzer"));
    CHECK(both.detail.contains("4 compile(s)"));
}

TEST_CASE("Only a refusal over an argument is this row's", "[node][conditions][refused-arguments]")
{
    // The observer hears every refusal the runner makes. A fingerprint nobody serves, or a scratch
    // disk that will not take a file, is another operator problem with its own counter, and naming
    // it here would send somebody to --allow-compile-arg for a fault no flag can fix.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };

    report.OnJobRefused(Cc::JobError { .reason = Cc::JobRefusal::UnknownFingerprint, .detail = {}, .subjects = {} });
    report.OnJobRefused(Cc::JobError { .reason = Cc::JobRefusal::ScratchUnavailable, .detail = {}, .subjects = {} });

    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);
    CHECK(logger.Snapshot().empty());
}

TEST_CASE("Changing the allowlist re-judges the row: what it admits leaves, the rest stays",
          "[node][conditions][refused-arguments]")
{
    // Latched and live are drawn apart on every surface, and this is why the row is live: the
    // operator's reload IS the fix, so *fixed* must read differently from *still refused*. And
    // re-judged rather than cleared, because a change that admits something ELSE -- an unrelated
    // entry, a misspelled one -- would otherwise read `clear` over an argument still refused.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("/volatile:iso", Cc::Flavor::Cl));
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("/volatile:iso", Cc::Flavor::Cl));
    report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fanalyzer", Cc::Flavor::Gcc));
    REQUIRE(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);

    SECTION("an unrelated or misspelled entry changes nothing the row says")
    {
        report.AllowlistChanged({ "/volatile:ms", "-fanalyser" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        auto const row = RefusedRow(conditions);
        CHECK(row.detail.contains("/volatile:iso"));
        CHECK(row.detail.contains("-fanalyzer"));
        CHECK(row.detail.contains("3 compile(s)"));
    }

    SECTION("an entry admitting one argument takes it, and its count, off the row")
    {
        report.AllowlistChanged({ "/volatile:iso" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        auto const row = RefusedRow(conditions);
        CHECK_FALSE(row.detail.contains("/volatile:iso"));
        CHECK(row.detail.contains("-fanalyzer"));
        CHECK(row.detail.contains("1 compile(s)"));
    }

    SECTION("entries admitting every argument clear it")
    {
        report.AllowlistChanged({ "/volatile:iso", "-fanalyzer" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);

        // A job that copied the OLD list before the change and reports after it names an
        // argument the list in force admits: it is judged by that list, and the row stays clear.
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fanalyzer", Cc::Flavor::Gcc));
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);

        // And the list taking an entry back out makes a later refusal count from one again.
        report.AllowlistChanged({ "/volatile:iso" });
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fanalyzer", Cc::Flavor::Gcc));
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        CHECK(RefusedRow(conditions).detail.contains("1 compile(s)"));
    }

    SECTION("an entry naming an argument no list can lift keeps it on the row")
    {
        // `-Xclang` is a row of `DeniedArguments`: the worker refuses it whatever the operator
        // lists, so the list naming it admits nothing and the row must go on saying so.
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-Xclang", Cc::Flavor::Clang));
        report.AllowlistChanged({ "-Xclang", "/volatile:iso", "-fanalyzer" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        CHECK(RefusedRow(conditions).detail.contains("-Xclang"));
    }

    SECTION("an entry naming a flag the worker refuses before its list keeps it on the row")
    {
        // `-fmodule-output=` writes a second artefact, which the worker refuses by its
        // side-artefact table BEFORE the operator's list is consulted -- and which has no row of
        // `DeniedArguments`. So a report asking only that table would drop it here while every
        // compile carrying it goes on being refused. The worker's own predicate is what answers.
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fmodule-output=x.pcm", Cc::Flavor::Gcc));
        report.AllowlistChanged({ "-fmodule-output=x.pcm", "/volatile:iso", "-fanalyzer" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        CHECK(RefusedRow(conditions).detail.contains("-fmodule-output=x.pcm"));
        CHECK(RefusedRow(conditions).detail.contains("1 compile(s)"));

        // And a late job carrying it is still counted, for the same reason.
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fmodule-output=x.pcm", Cc::Flavor::Gcc));
        CHECK(RefusedRow(conditions).detail.contains("2 compile(s)"));
    }

    SECTION("a refusal that cannot say which driver judged it is never admitted")
    {
        // A name the refusal had to cut is not the argument, so there is no question to re-ask:
        // the list naming it clears nothing.
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming("-fodd", std::nullopt));
        report.AllowlistChanged({ "-fodd", "/volatile:iso", "-fanalyzer" });
        CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
        CHECK(RefusedRow(conditions).detail.contains("-fodd"));
    }
}

TEST_CASE("A compile refused over several arguments names each and is counted once", "[node][conditions][refused-arguments]")
{
    // #1681. A refusal names EVERY argument its job carried that the worker will not pass, and it
    // is still ONE compile. A count summing each argument's refusals would say three compiles for
    // one; and a compile stays refused while ANY of its arguments is, so admitting one of them must
    // not take the compile off the count.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };
    std::vector<std::string> const both { "-fnew-a", "-fnew-b" };
    std::vector<std::string> const one { "-fnew-a" };
    report.OnJobRefused(Cc::JobError::RejectedArgumentsNaming(one, Cc::Flavor::Gcc));
    report.OnJobRefused(Cc::JobError::RejectedArgumentsNaming(both, Cc::Flavor::Gcc));

    auto row = RefusedRow(conditions);
    CHECK(row.detail.contains("-fnew-a"));
    CHECK(row.detail.contains("-fnew-b"));
    CHECK(row.detail.contains("2 compile(s)"));
    // Logged once per ARGUMENT, so the second refusal's new argument is said and the repeat is not.
    CHECK(LinesMentioning(logger, "-fnew-a") == 1);
    CHECK(LinesMentioning(logger, "-fnew-b") == 1);

    // Admitting `-fnew-a` clears the first compile and leaves the second, which is still refused
    // over `-fnew-b`.
    report.AllowlistChanged({ "-fnew-a" });
    REQUIRE(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
    row = RefusedRow(conditions);
    CHECK_FALSE(row.detail.contains("-fnew-a"));
    CHECK(row.detail.contains("-fnew-b"));
    CHECK(row.detail.contains("1 compile(s)"));

    // And admitting the rest clears the row.
    report.AllowlistChanged({ "-fnew-a", "-fnew-b" });
    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Clear);
}

TEST_CASE("A refusal one of whose arguments the list in force admits names only the rest",
          "[node][conditions][refused-arguments]")
{
    // The late-job rule per argument: a job judged by an older list names an argument the operator
    // has since admitted beside one still refused. The row names the second, counts the compile,
    // and never lists the first as refused.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, { "-fnew-a" } };
    std::vector<std::string> const both { "-fnew-a", "-fnew-b" };
    report.OnJobRefused(Cc::JobError::RejectedArgumentsNaming(both, Cc::Flavor::Gcc));
    auto const row = RefusedRow(conditions);
    CHECK_FALSE(row.detail.contains("-fnew-a"));
    CHECK(row.detail.contains("-fnew-b"));
    CHECK(row.detail.contains("1 compile(s)"));
}

TEST_CASE("The row says how many named arguments it could not fit", "[node][conditions][refused-arguments]")
{
    // Every named argument of one refusal, long enough that the row cannot hold them all: the
    // detail stays under its ceiling and counts the rest rather than stopping mid-name.
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };
    std::vector<std::string> arguments;
    for (auto const index: std::views::iota(std::size_t { 0 }, RefusedArgumentsReport::MaxNamedArguments))
        arguments.push_back(std::format("-fa-refused-argument-long-enough-to-crowd-the-row-{:02}", index));
    report.OnJobRefused(Cc::JobError::RejectedArgumentsNaming(arguments, Cc::Flavor::Gcc));

    auto const row = RefusedRow(conditions);
    CHECK(row.detail.size() <= Wire::MaxConditionDetailBytes);
    CHECK(row.detail.contains("1 compile(s)"));
    auto const said = static_cast<std::size_t>(
        std::ranges::count_if(arguments, [&](std::string const& argument) { return row.detail.contains(argument); }));
    REQUIRE(said > 0);
    REQUIRE(said < arguments.size());
    CHECK(row.detail.ends_with(std::format(" and {} more", arguments.size() - said)));
}

TEST_CASE("Past the cap, arguments are counted and neither named nor logged", "[node][conditions][refused-arguments]")
{
    // A client choosing a fresh argument per job must not be able to fill this node's log or grow
    // its memory, so the report names a bounded set and counts the rest -- and says ONCE that it
    // has stopped naming, rather than going quiet in a way that reads as "no more refusals".
    NodeConditions conditions;
    CapturingLogger logger;
    RefusedArgumentsReport report { conditions, logger, {} };

    constexpr auto Beyond = RefusedArgumentsReport::MaxNamedArguments + 4;
    for (auto const i: std::views::iota(std::size_t { 0 }, Beyond))
        report.OnJobRefused(Cc::JobError::RejectedArgumentNaming(std::format("-fdistinct-{}", i), Cc::Flavor::Gcc));

    CHECK(logger.Snapshot().size() == RefusedArgumentsReport::MaxNamedArguments + 1);
    CHECK(LinesMentioning(logger, std::format("-fdistinct-{}", Beyond - 1)) == 0);
    auto const row = RefusedRow(conditions);
    CHECK(row.detail.contains(std::format("{} compile(s)", Beyond)));
    CHECK(row.detail.contains("(4 of them over arguments not named)"));
}

namespace
{

/// A process runner that must never be reached: every job these cases send is refused first.
class UnreachedRunner final: public Cc::IProcessRunner
{
  public:
    Cc::CompileRun RunCaptureCombined(std::span<std::string const> argv) override
    {
        return RunCaptureSplit(argv);
    }
    Cc::CompileRun RunCaptureSplit(std::span<std::string const> /*argv*/) override
    {
        ++spawned;
        return Cc::CompileRun { .exitCode = 1, .out = {}, .err = {} };
    }
    int spawned = 0;
};

} // namespace

TEST_CASE("A worker refusing an argument on the wire raises the row that names it", "[node][conditions][refused-arguments]")
{
    // The report is only as good as its wiring: a real `WorkerProtocol` answering a real COMPILE
    // frame over a real `CompileJobRunner`, with this report as its observer, exactly as the worker
    // tier builds it. The counter and the row must move together, from one refusal.
    NodeConditions conditions;
    CapturingLogger logger;
    AtomicMetricsSink metrics;
    UnreachedRunner runner;
    FastCache::Testing::ScratchDirectory scratch { "fc-refused-args" };
    Cc::CompileJobRunner jobs {
        runner, scratch.Path(), { { "msvc", R"(C:\MSVC\bin\Hostx64\x64\cl.exe)" } }, Cc::ToolchainSurvey::Completed()
    };
    RefusedArgumentsReport report { conditions, logger, {} };
    Cc::WorkerProtocol protocol {
        jobs, Cc::UncheckedLeaseValidator(), { Wire::IdentityCodec }, &Testing::TestWorkerKey(), metrics, report
    };

    std::string const source = "int main(){return 0;}";
    auto const args = std::vector<std::string> { "/O2", "/analyze:plugin" };
    std::vector<std::byte> argsField;
    for (auto const& arg: args)
    {
        auto const length = static_cast<std::uint32_t>(arg.size());
        for (auto const shift: { 24U, 16U, 8U, 0U })
            argsField.push_back(static_cast<std::byte>((length >> shift) & 0xFFU));
        for (auto const c: arg)
            argsField.push_back(static_cast<std::byte>(c));
    }
    // The framing is the worker's own decoder's to judge, so a hand-built list that it would read
    // differently fails here rather than as a refusal for the wrong reason.
    REQUIRE(Cc::DecodeArgs(argsField) == args);
    auto const frame = Wire::EncodeCompile(Wire::CompileRequest {
        .leaseToken = "l1",
        .fingerprint = "msvc",
        .args = argsField,
        .source =
            Wire::EncodeCodecEnvelope(Wire::IdentityCodec, static_cast<std::uint32_t>(source.size()), Wire::AsBytes(source)),
        .acceptedCodecs = { Wire::IdentityCodec },
        .sourceName = "a.cpp",
        .compileDir = {},
        .compileDirReplacement = {},
        .sourceRoot = {},
        .sourceRootReplacement = {} });

    auto const reply = protocol.Answer(frame);
    REQUIRE(reply.has_value());
    CHECK(runner.spawned == 0);
    CHECK(metrics.Read(IMetricsSink::Counter::WorkerJobsRefusedRejectedArgument) == 1);
    CHECK(conditions.StateOf(NodeCondition::RefusedCompileArguments) == Wire::ConditionState::Raised);
    CHECK(RefusedRow(conditions).detail.contains("/analyze:plugin"));
    CHECK(LinesMentioning(logger, "/analyze:plugin") == 1);
}
