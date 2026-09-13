// SPDX-License-Identifier: Apache-2.0
#include "LiveSession.hpp"

#include <cassert>

namespace FastCache::Cli
{

Task<DashboardExit> RunLiveSession(IDashboardEventSource* events,
                                   IViewLadder* ladder,
                                   IFrameSink* sink,
                                   DashboardLimits limits)
{
    // Before the loop and outside it: the loop draws per frame, and this must not.
    auto const view = ladder->Decide();
    assert(view != nullptr && "a view ladder always has a bottom rung");
    co_return co_await RunDashboard(events, view.get(), sink, limits);
}

} // namespace FastCache::Cli
