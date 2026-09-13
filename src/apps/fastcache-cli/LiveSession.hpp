// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"
#include "DashboardLoop.hpp"

#include <FastCache/Async/Task.hpp>

#include <memory>

namespace FastCache::Cli
{

/// @file LiveSession.hpp
/// One `live-stats` session: decide how it draws, once, then run the loop.

/// How a session draws, decided from what the terminal can do.
///
/// **Asked once per session, never per frame** (#134 §9.12). What a terminal can do is
/// settled by asking it -- a DA1 reply, a mode report -- and a frame that asked again would
/// interleave those queries with the frames it draws, once per frame. A terminal does not
/// grow Sixel mid-session, so the answer is not worth re-asking either.
/// A seam rather than a call, so that "once" is COUNTED by a test instead of being a
/// property of where a line happens to sit.
///
/// Production's implementation maps the capabilities the started terminal reported
/// through the rung chooser to the view for that rung. A run with no terminal has one rung
/// and decides it the same way, which keeps the count honest for both compositions.
class IViewLadder
{
  public:
    IViewLadder() = default;
    IViewLadder(IViewLadder const&) = delete;
    IViewLadder(IViewLadder&&) = delete;
    IViewLadder& operator=(IViewLadder const&) = delete;
    IViewLadder& operator=(IViewLadder&&) = delete;
    virtual ~IViewLadder() = default;

    /// Decide how this session draws.
    /// @return The view every frame of the session is drawn through; never null.
    [[nodiscard]] virtual std::unique_ptr<IDashboardView> Decide() = 0;
};

/// Run one session: decide the view, then run the loop over @p events with it.
///
/// Pointers rather than references, because this is a coroutine and its frame outlives
/// the call that created it. None may be null.
/// @param events Where the session's events come from.
/// @param ladder How the session draws; asked exactly once, before the first event.
/// @param sink Where frames go.
/// @param limits What bounds the session.
/// @return How the session ended.
[[nodiscard]] Task<DashboardExit> RunLiveSession(IDashboardEventSource* events,
                                                 IViewLadder* ladder,
                                                 IFrameSink* sink,
                                                 DashboardLimits limits);

} // namespace FastCache::Cli
