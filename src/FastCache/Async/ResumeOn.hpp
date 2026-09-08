// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Async/IExecutor.hpp>
#include <FastCache/Async/Task.hpp>

#include <coroutine>

namespace FastCache
{

/// Awaitable that continues the awaiting coroutine somewhere else.
///
/// `co_await ResumeOn{executor}` suspends and posts the handle via
/// `IExecutor::Submit` (thread-safe); the coroutine resumes wherever that executor
/// runs things. Used to hand a freshly accepted connection from the acceptor thread
/// to the single-threaded reactor that will own it -- so the connection's coroutine
/// only ever runs on that one thread -- and to move a blocking, seconds-long compile
/// off the accept loop onto a pool, which is the same move written the same way.
///
/// An `IExecutor` rather than an `IReactor`, because this only ever needed `Submit`
/// and a pool is not a reactor. Every `co_await ResumeOn{someReactor}` still
/// compiles: `IReactor` derives from `IExecutor`.
struct ResumeOn
{
    IExecutor& target; ///< Where the coroutine should continue.

    /// Never ready — always suspend so the resumption happens on the target.
    [[nodiscard]] bool await_ready() const noexcept
    {
        return false;
    }

    /// Post the handle to the target for resumption.
    ///
    /// Templated on the promise for `SleepUntil`'s reason: an executor destroyed before
    /// it runs this has to be told whether anything else can free the chain, and only
    /// the parking coroutine's own promise type knows
    /// ([#1025](https://github.com/LASTRADA-Software/fastcached/issues/1025)).
    /// @tparam Promise The suspending coroutine's promise type.
    /// @param handle The suspended coroutine to resume there.
    template <typename Promise>
    void await_suspend(std::coroutine_handle<Promise> handle) const
    {
        target.Submit(Detail::ParkedWorkFor(handle));
    }

    void await_resume() const noexcept {}
};

} // namespace FastCache
