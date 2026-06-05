// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IReactor.hpp>

#include <coroutine>

namespace FastCache
{

/// Awaitable that re-schedules the awaiting coroutine onto a specific reactor's
/// thread. `co_await ResumeOn{reactor}` suspends and posts the coroutine handle
/// via `IReactor::Submit` (thread-safe); the coroutine resumes on that
/// reactor's loop thread. Used to hand a freshly accepted connection from the
/// acceptor thread to the single-threaded reactor that will own it, so the
/// connection's coroutine only ever runs on that one thread.
struct ResumeOn
{
    IReactor& reactor; ///< Reactor whose thread the coroutine should continue on.

    /// Never ready — always suspend so the resumption happens on the target.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    /// Post the handle to the target reactor for resumption on its thread.
    /// @param handle The suspended coroutine to resume on the reactor.
    void await_suspend(std::coroutine_handle<> handle) const
    {
        reactor.Submit(handle);
    }

    void await_resume() const noexcept {}
};

} // namespace FastCache
