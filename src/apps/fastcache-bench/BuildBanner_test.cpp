// SPDX-License-Identifier: Apache-2.0
/// The banner's DECISION, driven over staged readings
/// ([#1439](https://github.com/LASTRADA-Software/fastcached/issues/1439)).
///
/// A build's own figures can only ever exercise one arm -- this binary is either optimised or
/// it is not -- so the arm that matters is always the one the host cannot produce. Every case
/// below therefore hands `ReasonFor` a record rather than asking the compiler, which is what
/// makes *tested both ways* possible at all. The one case that does ask the compiler asks it
/// about ACQUISITION, and it is the only one that can.
///
/// These cases are not tagged `[!benchmark]`, so they are the only non-hidden, non-benchmark
/// cases in this binary: `ctest -R bench-build-banner` runs `fastcache-bench [buildbanner]`
/// and asserts that a non-zero number of cases ran, because a tag that has stopped matching
/// reports `No tests ran` and exits non-zero, which reads as a failing subject.

#include "BuildBanner.hpp"

#include <FastCache/Core/EnumTable.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <initializer_list>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace FastCache;
using namespace FastCache::Bench;

namespace
{

/// A staged reading, so a case can describe a build this host cannot produce.
///
/// @param label What `CMAKE_BUILD_TYPE` would have said. Decides nothing, which is what one
///              case below exists to prove.
/// @param evidence Whether this compiler states `__OPTIMIZE__` at all.
/// @param stated The facts the compiler stated. Order affects only the printed list.
/// @return The reading.
[[nodiscard]] BuildConfiguration Reading(std::string_view label,
                                         OptimiserEvidence evidence,
                                         std::initializer_list<BuildFact> stated)
{
    return BuildConfiguration { .buildTypeLabel = label,
                                .compiler = "a compiler that does not exist",
                                .stated = std::vector<BuildFact>(stated),
                                .optimiserEvidence = evidence };
}

/// A build with nothing wrong with it: asserts out, optimiser confirmed, nothing instrumented.
/// @return The reading.
[[nodiscard]] BuildConfiguration OptimisedReading()
{
    return Reading("Release", OptimiserEvidence::Stated, { BuildFact::AssertsCompiledOut, BuildFact::OptimiserRan });
}

} // namespace

TEST_CASE("A bench figure's standing is decided by what the compiler stated and never by the build type label",
          "[buildbanner]")
{
    // #1420's misreading in one assertion: an `cl-release` log was read as a Debug signature,
    // and the only tell was the build directory's name. A banner that believed its label
    // would answer differently for these two readings, which are the same build.
    auto const labelledRelease = Reading("Release", OptimiserEvidence::Stated, {});
    auto const labelledDebug = Reading("Debug", OptimiserEvidence::Stated, {});

    CHECK(ReasonFor(labelledRelease) == StandingReason::AssertsLive);
    CHECK(ReasonFor(labelledDebug) == StandingReason::AssertsLive);
    CHECK(StandingFor(ReasonFor(labelledRelease)) == Standing::NotACost);

    // And the label is still PRINTED, because an operator needs to know what was asked for.
    CHECK(RenderBuildBanner(labelledRelease).contains("Release"));
}

TEST_CASE("Every reason a bench figure's standing can rest on is reachable and names what was observed", "[buildbanner]")
{
    struct Row
    {
        std::string_view what;      ///< What this reading is.
        BuildConfiguration reading; ///< The staged reading.
        StandingReason reason;      ///< The reason it must produce.
        Standing standing;          ///< What that reason makes of a figure.
    };

    std::vector<Row> const rows {
        { .what = "gcc -O0, no NDEBUG",
          .reading = Reading("Debug", OptimiserEvidence::Stated, { BuildFact::InliningOff }),
          .reason = StandingReason::AssertsLive,
          .standing = Standing::NotACost },
        { .what = "gcc -O0 -DNDEBUG: NDEBUG alone is not optimisation",
          .reading = Reading("Release", OptimiserEvidence::Stated, { BuildFact::AssertsCompiledOut }),
          .reason = StandingReason::OptimiserDidNotRun,
          .standing = Standing::NotACost },
        { .what = "cl /Od /RTC1 /DNDEBUG: /RTC settles what the missing optimiser macro cannot",
          .reading = Reading("Debug",
                             OptimiserEvidence::NotStatedByThisCompiler,
                             { BuildFact::AssertsCompiledOut, BuildFact::MsvcRuntimeChecks }),
          .reason = StandingReason::OptimiserDidNotRun,
          .standing = Standing::NotACost },
        { .what = "cl /O2 /MDd: optimised against the debug runtime",
          .reading = Reading("RelWithDebInfo",
                             OptimiserEvidence::NotStatedByThisCompiler,
                             { BuildFact::AssertsCompiledOut, BuildFact::MsvcDebugRuntime }),
          .reason = StandingReason::CheckedRuntime,
          .standing = Standing::NotACost },
        { .what = "clang -O2 -DNDEBUG -fsanitize=address",
          .reading = Reading("Release",
                             OptimiserEvidence::Stated,
                             { BuildFact::AssertsCompiledOut, BuildFact::OptimiserRan, BuildFact::AddressSanitizer }),
          .reason = StandingReason::SanitizerInstrumented,
          .standing = Standing::NotACost },
        { .what = "cl /O2 /DNDEBUG: nothing is wrong and nothing confirms the optimiser",
          .reading = Reading("Release", OptimiserEvidence::NotStatedByThisCompiler, { BuildFact::AssertsCompiledOut }),
          .reason = StandingReason::OptimiserUnstated,
          .standing = Standing::Unconfirmed },
        { .what = "clang -O2 -DNDEBUG",
          .reading = OptimisedReading(),
          .reason = StandingReason::Confirmed,
          .standing = Standing::ACost },
    };

    std::vector<bool> covered(EnumeratorCount<StandingReason>, false);
    for (Row const& row: rows)
    {
        INFO("reading: " << row.what);
        CHECK(ReasonFor(row.reading) == row.reason);
        CHECK(StandingFor(row.reason) == row.standing);
        covered[static_cast<std::size_t>(row.reason)] = true;
    }

    // A row nobody staged is an arm nothing tested, and the table above is the only thing
    // that would say so: `RowsInEnumeratorOrder` proves the row EXISTS, never that a reading
    // reaches it.
    for (std::size_t const index: std::views::iota(std::size_t { 0 }, covered.size()))
    {
        INFO("StandingReason enumerator " << index << ", whose sentence is \"" << StandingTable[index].sentence << '"');
        CHECK(covered[index]);
    }
}

TEST_CASE("The reason a bench figure is refused is the EARLIEST that applies, so the worst news wins", "[buildbanner]")
{
    // Both rules apply. Precedence is the enumerator order of `StandingReason`, and an
    // implementation that answered `SanitizerInstrumented` here would be telling an operator
    // to rebuild without sanitizers when the asserts are the larger problem.
    auto const both = Reading("Debug", OptimiserEvidence::Stated, { BuildFact::AddressSanitizer });
    CHECK(ReasonFor(both) == StandingReason::AssertsLive);
}

TEST_CASE("An unoptimised bench build says so unmistakably, and a confirmed one does not shout", "[buildbanner]")
{
    std::string const unoptimised = RenderBuildBanner(Reading("Debug", OptimiserEvidence::Stated, {}));
    std::string const optimised = RenderBuildBanner(OptimisedReading());

    CHECK(unoptimised.contains("NOT A COST"));
    CHECK(unoptimised.contains("!!!!!!!!!!"));
    CHECK(unoptimised.contains("NDEBUG is not defined"));

    // The half that distinguishes: a banner that shouted unconditionally would satisfy every
    // assertion above while saying nothing.
    CHECK_FALSE(optimised.contains("NOT A COST"));
    CHECK_FALSE(optimised.contains("!!!"));
    CHECK(optimised.contains("these figures are a cost"));

    // An unconfirmed build is neither of those two, which is the third state a bool could not
    // carry.
    std::string const unconfirmed =
        RenderBuildBanner(Reading("Release", OptimiserEvidence::NotStatedByThisCompiler, { BuildFact::AssertsCompiledOut }));
    CHECK(unconfirmed.contains("UNCONFIRMED"));
    CHECK_FALSE(unconfirmed.contains("NOT A COST"));
    CHECK_FALSE(unconfirmed.contains("these figures are a cost"));
}

TEST_CASE("Every bench banner names which column is the per-operation cost", "[buildbanner]")
{
    // The other half of #1420's misreading: `est run time` sits beside the case name and is
    // the larger number, so it is the one a reader reaches for.
    std::string const banner = RenderBuildBanner(OptimisedReading());
    CHECK(banner.contains("mean"));
    CHECK(banner.contains("est run time"));

    // And the note is NOT one of the banner's indented fact lines. `bench/inproc_bench.py`
    // reads those back into its report's environment block by their two-space indent, so a
    // sentence of advice indented like a fact would become a row describing this prose.
    CHECK(banner.contains("\nreading a figure: "));
    CHECK_FALSE(banner.contains("\n  reading a figure: "));

    // The control on the same contract, from the other side: every fact line IS indented.
    for (BannerLine const& line: BannerLines())
    {
        INFO("banner line: " << line.label);
        CHECK(banner.contains(std::format("\n  {}: ", line.label)));
    }
}

TEST_CASE("A bench figure's own line names the mean, the sample count and the build's standing", "[buildbanner]")
{
    Figure const figure {
        .name = "control-unordered-map", .meanNanoseconds = 5.4, .estimatedNanoseconds = 450789.0, .samples = 150
    };

    std::string const refused = RenderFigure(figure, StandingReason::AssertsLive);
    CHECK(refused.contains("control-unordered-map"));
    CHECK(refused.contains("mean = 5.40 ns/op"));
    CHECK(refused.contains("150 sample(s)"));
    CHECK(refused.contains("[NOT A COST]"));
    CHECK(refused.contains("est run time 450789.00 ns is the whole benchmark and not the cost"));

    // Same figure, sound build: only the marker moves, so a reader can tell the two runs apart
    // from one line.
    std::string const accepted = RenderFigure(figure, StandingReason::Confirmed);
    CHECK(accepted.contains("[a cost]"));
    CHECK_FALSE(accepted.contains("[NOT A COST]"));
}

TEST_CASE("This bench binary reports the build it was itself compiled with", "[buildbanner]")
{
    // Acquisition, which no staged reading can cover. The comparisons are against the macros
    // THIS translation unit sees -- it is compiled into the same target as
    // `CurrentBuildConfiguration`, with the same flags -- so a reading that came back
    // default-constructed, or that read a CMake variable instead of the compiler, fails here.
    BuildConfiguration const reading = CurrentBuildConfiguration();

    CHECK_FALSE(reading.compiler.empty());
    CHECK(reading.compiler != "a compiler that does not exist");

#if defined(NDEBUG)
    CHECK(Stated(reading, BuildFact::AssertsCompiledOut));
    CHECK(AssertsOf(reading) == Asserts::CompiledOut);
#else
    CHECK_FALSE(Stated(reading, BuildFact::AssertsCompiledOut));
    CHECK(AssertsOf(reading) == Asserts::Live);
#endif

#if defined(__OPTIMIZE__)
    CHECK(Stated(reading, BuildFact::OptimiserRan));
    CHECK(OptimiserOf(reading) == Optimiser::Ran);
#else
    CHECK_FALSE(Stated(reading, BuildFact::OptimiserRan));
#endif

#if defined(__clang__) || defined(__GNUC__)
    CHECK(reading.optimiserEvidence == OptimiserEvidence::Stated);
#else
    CHECK(reading.optimiserEvidence == OptimiserEvidence::NotStatedByThisCompiler);
#endif

    // The banner is what gets printed, so it is what is checked -- not only the record.
    std::string const banner = RenderBuildBanner(reading);
    CHECK(banner.starts_with("fastcache-bench: the build these figures come from\n"));
    CHECK(banner.contains(reading.compiler));
    CHECK(banner.contains("these figures are "));
}
