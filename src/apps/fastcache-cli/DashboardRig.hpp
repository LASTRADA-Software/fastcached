// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardLoop.hpp"
#include "ScriptedDashboardEvents.hpp"

#include <FastCache/Async/TestReactor.hpp>
#include <FastCache/Core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tests/Unwrap.hpp>

namespace FastCache::Cli::Testing
{

/// @file DashboardRig.hpp
/// Driving the dashboard loop from a script, shared by every test that needs a model the FOLD
/// produced.
///
/// **Shared because a hand-built model is the fixture that re-acquires what production binds
/// once.** A panel test that assembled `DashboardModel::history` itself would test a model no run
/// can produce -- and would stay green while the fold and the panel came to disagree. So the panel
/// tests draw through the real loop, with the view under test as the loop's view, exactly as a
/// session composes it.

/// Collects frames so a case can compare runs to each other.
class CollectingSink final: public IFrameSink
{
  public:
    void PresentPlaced(DashboardFrame const& frame) override
    {
        frames.push_back(frame.text);
        placements.push_back(frame.placements);
    }

    std::vector<std::string> frames {};                     ///< Each frame's text.
    std::vector<std::vector<FramePlacement>> placements {}; ///< Each frame's images, index for index with `frames`.
};

/// Run the dashboard once, writing the result where the caller can read it.
///
/// A named coroutine over POINTERS rather than a capturing lambda: a lambda coroutine destroys its
/// closure at the first suspension, so every captured reference dangles from then on -- and this
/// coroutine suspends on its first statement. The caller owns every argument for the whole run.
/// @param events The scripted source.
/// @param reader What reads a `Sample`.
/// @param view What draws.
/// @param sink Where frames go.
/// @param limits What bounds the run.
/// @param out Where to put the result.
/// @return The task to submit.
[[nodiscard]] inline Task<void> DriveOnce(IDashboardEventSource* events,
                                          SampleReader reader,
                                          IDashboardView* view,
                                          IFrameSink* sink,
                                          DashboardLimits limits,
                                          std::optional<DashboardExit>* out)
{
    *out = co_await RunDashboard(events, reader, view, sink, limits);
}

/// Drive the loop to completion on a deterministic reactor.
/// @param script The events, in order.
/// @param limits What bounds the run.
/// @param view The view to draw through.
/// @param sink Where frames go.
/// @param reader What reads a `Sample`; the stats ladder's own, as a `cache` session binds it.
/// @return How it ended.
[[nodiscard]] inline DashboardExit Drive(std::vector<DashboardEvent> script,
                                         DashboardLimits limits,
                                         IDashboardView& view,
                                         CollectingSink& sink,
                                         SampleReader reader = &ReadStatsSample)
{
    auto clock = ManualClock {};
    auto reactor = TestReactor { clock };
    auto events = ScriptedDashboardEvents { reactor, std::move(script) };

    auto result = std::optional<DashboardExit> {};
    auto task = DriveOnce(&events, reader, &view, &sink, limits, &result);

    reactor.Submit(task.Native());
    reactor.Drain();

    REQUIRE(result.has_value());
    return FastCache::Testing::Unwrap(result);
}

} // namespace FastCache::Cli::Testing
