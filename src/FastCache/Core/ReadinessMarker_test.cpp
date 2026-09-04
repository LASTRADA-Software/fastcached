// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ReadinessMarker.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>

using namespace FastCache;

// Every readiness line this repository publishes is waited on by something this
// build does not recompile, so each is pinned by its BYTES below.
//
// ## Why a symbol both ends spell is not enough
//
// A wire constant has two facts, its name and its value, and a test that reads the
// constant can only check the first: change `ReadinessMarkerTable`'s text and every
// in-tree reader agrees with itself perfectly while every fixture stops matching.
// That is the rule `wire-and-protocol.md` states for an opcode, and these literals
// are in the same position -- the difference being that an opcode's other end is a
// deployed binary and this one's is four fixtures in two languages plus a workflow
// step.
//
// So the assertions below deliberately restate the literal. It looks like the code
// smell it is normally right to remove, and it is the anchor: it is the only thing
// in the repository that fails when the published text changes.
//
// ## What a failure here means
//
// Not "a string changed". It means the listed waiters are about to fail by TIMEOUT
// against a line nothing writes -- the slowest and least informative failure mode,
// each reported against whatever that fixture happened to be waiting for. If the
// reword is intended, every waiter named beside the assertion has to change in the
// same commit, and none of them is reached by this build.

TEST_CASE("The compile node's readiness marker keeps the bytes four fixtures wait on", "[core][readiness-marker]")
{
    // #654. Waiters, measured rather than remembered:
    //   scripts/lib/e2e-common.sh          E2eNodeReadyMarker, used by wait_for_node_ready
    //   scripts/node-scratch-isolation-e2e.ps1   a hardcoded literal (PowerShell; no access
    //                                            to the bash library)
    //   .github/workflows/build.yml        two `grep -q` steps
    //   scripts/check-e2e-helpers.sh       its stand-in node prints this line, and a case
    //                                      name references it
    // Plus prose in docs/how-it-works.md, docs/getting-started/distributed-compilation.md,
    // .agent/rules/testing.md, .agent/rules/distributed-compilation.md and AGENT.md, which
    // go stale rather than break.
    CHECK(ReadinessMarkerText(ReadinessMarker::CompileNode) == "compile node ready");
}

TEST_CASE("The daemon's readiness marker keeps the bytes its two waiters grep for", "[core][readiness-marker]")
{
    // #646 changed what this line MEANS while deliberately keeping what it SAYS,
    // which is only safe as long as something holds the bytes.
    //   bench/runner.py                READY_MARKER
    //   .github/workflows/build.yml    the packaged-service step's `grep -q`
    CHECK(ReadinessMarkerText(ReadinessMarker::Daemon) == "ready, accepting connections");
}

TEST_CASE("No readiness marker claims to report a bind", "[core][readiness-marker]")
{
    // #652: `node-scratch-isolation-e2e.ps1` waited on the node's marker and reported
    // a BIND failure, and two rulebook files described that marker as *bound*. Both
    // markers are logged strictly after their listener is already accepting, so
    // "bound" is a fact that was true long before either line appeared -- an operator
    // sent to check the port finds it open and has been pointed at the one thing that
    // is working.
    //
    // The guard is structural rather than an assertion about spelling: `ReadinessFact`
    // has no `Bound` enumerator, so no row can claim it. What is left to assert is
    // that every row states SOMETHING -- `Unstated` is the zero value precisely so an
    // omitted column is not silently readable as a verdict (#736's `0 of 0`).
    for (auto const& row: ReadinessMarkerTable)
    {
        CHECK(row.fact != ReadinessFact::Unstated);
        CHECK_FALSE(row.text.empty());
        CHECK_FALSE(row.meaning.empty());
    }

    // And the two rows differ, which is the distinction #652 turns on: the daemon's
    // line promises its acceptors are armed and nothing more, while the node's also
    // promises the heartbeat thread is running. A fixture that treats them as the
    // same signal is waiting on the weaker of the two.
    CHECK(ReadinessFactOf(ReadinessMarker::Daemon) == ReadinessFact::Accepting);
    CHECK(ReadinessFactOf(ReadinessMarker::CompileNode) == ReadinessFact::Serving);
}

TEST_CASE("Each marker's meaning names the ordering a fixture author would get wrong", "[core][readiness-marker]")
{
    // The `meaning` column is a forcing function, not decoration: it is read at the
    // moment somebody reaches for a marker as a signal for something it does not
    // promise, which is how #652 happened. Asserting it is non-empty (above) does not
    // stop it becoming a restatement of the enumerator, so the two facts a reader
    // needs are pinned here.
    //
    // For the node that is the ordering -- bound < accepting < READY < surveyed -- and
    // the survey in particular, because #365 moved the survey off the startup path and
    // that is what made the line stop meaning *surveyed* without anybody renaming it.
    auto const node = ReadinessMarkerTable[static_cast<std::size_t>(ReadinessMarker::CompileNode)].meaning;
    CHECK(node.contains("heartbeat"));
    CHECK(node.contains("survey"));
    CHECK(node.contains("Later than the bind"));

    // For the daemon, that the acceptors are armed rather than merely bound.
    auto const daemon = ReadinessMarkerTable[static_cast<std::size_t>(ReadinessMarker::Daemon)].meaning;
    CHECK(daemon.contains("armed"));
    CHECK(daemon.contains("later than the bind"));
}
