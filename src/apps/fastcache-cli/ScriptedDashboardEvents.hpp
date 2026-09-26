// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "DashboardEvent.hpp"

#include <coroutine>
#include <cstddef>
#include <utility>
#include <vector>

#include <core/async/Task.hpp>
#include <core/net/EventLoop.hpp>

namespace FastCache::Cli::Testing
{

/// @file ScriptedDashboardEvents.hpp
/// A dashboard event source that is a LIST.
///
/// This is the fake the whole *100% mockable* constraint rests on, so two things about
/// it are load-bearing rather than incidental.
///
/// **It PARKS.** Every `Next()` suspends through the injected reactor before it answers,
/// even though the answer is sitting in a vector. A source that resolved synchronously
/// would satisfy `IDashboardEventSource` and make every property defined by waiting
/// vacuous while appearing to pass -- a tick arriving while a fetch is outstanding, a
/// quit during a slow sample. The rule is stated on `ITerminalChannel::Read` for the
/// same reason and it is the one this file exists to honour: **a fake that resolves
/// what production suspends on cannot exercise a suspension protocol.**
///
/// It parks through `core::net::EventLoop::Schedule` -- the same door production parks through --
/// so under `core::net::testing::TestLoop` the resumption is still placed by the test rather than raced.
/// Determinism comes from the reactor being deterministic, not from the fake being
/// synchronous, which is the distinction that makes this honest.
///
/// **It runs out rather than repeating.** Past the end it answers `Detached`, so a loop
/// that asks for one more event than the script holds ends rather than spinning, and a
/// script that was too short says so by ending early rather than by looping forever.

/// Suspend and be resumed by the reactor, with no delay.
///
/// A zero delay rather than a real one: the point is that resumption goes through the
/// reactor's queue, not that time passes. `core::net::testing::TestLoop` therefore PLACES it, and a
/// production reactor costs one turn of its loop.
///
/// It borrows the handle rather than taking `core::async::ParkedWork`, which is correct here and
/// worth saying because the choice looks arbitrary: the awaiting chain is rooted in the
/// task the caller owns and awaits, not in a `core::async::DetachedTask`, so nothing but that caller
/// may free it -- which is exactly when `core::net::EventLoop::Schedule`'s borrowing overload is the
/// right one.
struct ParkAwaiter
{
    core::net::EventLoop& reactor;

    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor.schedule(reactor.clock().now(), handle);
    }

    void await_resume() const noexcept {}
};

class ScriptedDashboardEvents final: public IDashboardEventSource
{
  public:
    /// Answer from @p script, in order.
    /// @param reactor Where each answer parks before it is delivered.
    /// @param script The events, in the order they happen.
    ScriptedDashboardEvents(core::net::EventLoop& reactor, std::vector<DashboardEvent> script):
        _reactor { reactor },
        _script { std::move(script) }
    {
    }

    [[nodiscard]] core::async::Task<DashboardEvent> Next() override
    {
        // Park FIRST, unconditionally, including for the end-of-script answer. Parking
        // only when there is something to deliver would make the exhausted case the one
        // path that resolves synchronously -- and that is the path a quit takes, which
        // is exactly where a suspension bug would hide.
        co_await ParkAwaiter { .reactor = _reactor };

        if (_closed || _at >= _script.size())
            co_return DashboardEvent { .kind = DashboardEventKind::Detached,
                                       .note = _closed ? "the dashboard was closed" : "the script ran out of events" };

        co_return _script[_at++];
    }

    void Close() noexcept override
    {
        _closed = true;
    }

    /// How many events were actually taken.
    ///
    /// A script is a claim about what the loop would consume, and a case asserting only
    /// the frames cannot tell *consumed every event* from *stopped after two*. This is
    /// what lets a case say which.
    /// @return The count.
    [[nodiscard]] std::size_t Consumed() const noexcept
    {
        return _at;
    }

  private:
    core::net::EventLoop& _reactor;
    std::vector<DashboardEvent> _script;
    std::size_t _at { 0 };
    bool _closed { false };
};

} // namespace FastCache::Cli::Testing
