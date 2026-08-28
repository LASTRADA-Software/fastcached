// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coroutine>

namespace FastCache
{

/// Somewhere a suspended coroutine can be handed to be resumed.
///
/// The one thing `ResumeOn` needs, split out from `IReactor` so a thread pool can
/// offer it without pretending to be a reactor. A pool that ran compiles has no
/// timers, no cancellable deadlines and no clock, and implementing three methods
/// that answer nothing in order to reach a fourth is how an interface stops meaning
/// what it says.
///
/// **Threading contract: `Submit` is callable from any thread.** Where the handle
/// resumes is the implementation's business -- a reactor resumes it on its one loop
/// thread, a pool on whichever of its threads is free -- so a coroutine that has
/// awaited its way onto one must not assume it is still where it started.
class IExecutor
{
  public:
    IExecutor() = default;
    IExecutor(IExecutor const&) = delete;
    IExecutor(IExecutor&&) = delete;
    IExecutor& operator=(IExecutor const&) = delete;
    IExecutor& operator=(IExecutor&&) = delete;
    virtual ~IExecutor() = default;

    /// Post a coroutine handle for resumption.
    /// @param handle Coroutine to resume. Must remain alive until it is.
    virtual void Submit(std::coroutine_handle<> handle) = 0;
};

} // namespace FastCache
