// SPDX-License-Identifier: Apache-2.0
/// The banner is printed ONCE per executable, by a Catch2 listener
/// ([#1439](https://github.com/LASTRADA-Software/fastcached/issues/1439)).
///
/// ## Why a listener and not each bench file
///
/// The build configuration is a property of the BINARY, not of a benchmark, and this binary
/// has no `main` of ours to put a line in: it links `Catch2::Catch2WithMain`. That is the same
/// shape `cmake/ErrorPopups.cmake` met and settled the same way -- one translation unit
/// reaching every case, rather than a call each case has to remember. `RaftPeerFrameBench.cpp`
/// had already started down the per-file road with a `BuildKind` constant of its own, and it
/// had already gone wrong: it called a build with `NDEBUG` *optimised*, which `NDEBUG` does not
/// say. That constant is gone; this is the one answer.
///
/// ## Why STDERR
///
/// `bench/inproc_bench.py` parses this binary's STDOUT as XML. Measured on Catch2 3.6.0 (the
/// version this tree pins): under `--reporter xml` the XmlReporter redirects the streams a test
/// case writes, so `std::cout`/`std::cerr` from inside a case are captured into `<StdOut>` /
/// `<StdErr>` and the document still parses -- but C stdio and `std::println` are not
/// redirected, and text emitted from inside a `BENCHMARK` body lands inside an open start tag
/// and breaks the parse outright. stderr is safe for any API, so every line this binary writes
/// of its own goes there and the rule needs no exception for how it was written.
///
/// That is the whole binary's rule and not this listener's: **stdout belongs to the Catch2
/// reporter, and every line a bench file writes of its own goes to stderr.** The five bench
/// files were moved onto it with this change, `bench/inproc_bench.py` reads the `SCALING`
/// lines off stderr accordingly, and `ctest -R bench-build-banner-streams` refuses a
/// document whose `<StdOut>` carries any text -- so a `std::cout` added tomorrow fails a
/// check rather than being safe by luck about which reporter is running.
///
/// `testRunStarting` and `testRunEnded` run outside any test case, so they reach the real
/// stderr. `benchmarkEnded` runs inside the case, so under the XML reporter its line is
/// captured into that case's `<StdErr>` -- which is where a figure's own marker belongs
/// anyway. `ctest -R bench-build-banner-streams` asserts both halves, and that the document
/// carries no mixed content.
///
/// The cost of that choice, stated rather than left to be discovered: with the two streams
/// MERGED onto one terminal (`fastcache-bench 2>&1`), a per-figure line lands between the two
/// halves of the console reporter's own row, because the reporter writes a row's name and its
/// number in two writes with the measurement in between. Separate streams -- which is every
/// redirected, piped or reported run -- are unaffected. Moving these lines to stdout would fix
/// the cosmetics and reopen the parse hazard above, and moving them to a summary at the end
/// would separate a figure from its marker, which is the thing the marker is for.

#include "BuildBanner.hpp"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstddef>
#include <iostream>
#include <string>

namespace
{

/// Prints what build this executable is, before any case runs, and marks every figure with it.
class BuildBannerListener: public Catch::EventListenerBase
{
  public:
    using Catch::EventListenerBase::EventListenerBase;

    // No `getDescription()`. Catch2 detects it by SFINAE and falls back to
    // "(No description provided)" in `--list-listeners`, so it is optional -- and its name
    // is fixed by Catch2 while this tree's `readability-identifier-naming` requires
    // `CamelCase` for a function that is not an override. `NOLINT` is banned here, so the
    // choice is between widening the shared `FunctionIgnoredRegexp` in `.clang-tidy` for
    // one listing string and going without it. The overrides below are exempt because
    // clang-tidy does not rename a method that overrides a base one.

    /// Writes the banner before the first case runs.
    /// @param testRunInfo Unused; the banner describes the binary rather than the run.
    void testRunStarting(Catch::TestRunInfo const& testRunInfo) override
    {
        (void) testRunInfo;
        std::cerr << FastCache::Bench::RenderBuildBanner(_reading);
    }

    /// Writes one line per figure, naming the `mean` so no column can be mistaken for it.
    /// @param benchmarkStats What Catch2 measured.
    void benchmarkEnded(Catch::BenchmarkStats<> const& benchmarkStats) override
    {
        std::cerr << FastCache::Bench::RenderFigure(
            FastCache::Bench::Figure { .name = benchmarkStats.info.name,
                                       .meanNanoseconds = benchmarkStats.mean.point.count(),
                                       .estimatedNanoseconds = benchmarkStats.info.estimatedDuration,
                                       .samples = benchmarkStats.samples.size() },
            _reason);
    }

    /// Writes the verdict again, because a long run scrolls the banner off the screen.
    /// @param testRunStats Unused; the verdict is about the binary rather than about the results.
    void testRunEnded(Catch::TestRunStats const& testRunStats) override
    {
        (void) testRunStats;
        std::cerr << FastCache::Bench::RenderVerdict(_reading);
    }

  private:
    /// Read once: the answer cannot change while the process runs, and it allocates.
    FastCache::Bench::BuildConfiguration _reading = FastCache::Bench::CurrentBuildConfiguration();

    /// Derived once from `_reading`, for the per-figure marker.
    FastCache::Bench::StandingReason _reason = FastCache::Bench::ReasonFor(_reading);
};

} // namespace

CATCH_REGISTER_LISTENER(BuildBannerListener)
