// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <functional>

namespace FastCache::Cc
{

/// Runs a fixed number of independent slices and returns when all have finished.
///
/// This is the launcher's PARALLELISM seam, and it lives here beside
/// `IProcessRunner` for the same reason that one does: it is an app-local
/// injected seam for an ambient dependency, defined where its consumer lives,
/// with a fake in tests.
///
/// ## Why not `IExecutor`
///
/// `FastCache/Async/IExecutor.hpp` is the **coroutine-resumption** seam -- AGENT.md
/// calls it "the one thing `ResumeOn` needs" -- and its only method takes a
/// `std::coroutine_handle<>`. Its one implementation, `ThreadPoolExecutor`, offers
/// nothing else, `ResumeOn` is usable only from inside a coroutine, and `Task<T>` is
/// awaitable-only: nothing in this repository can drive a task to completion from
/// synchronous code. The toolchain walk is synchronous startup code, so reaching
/// `IExecutor` would mean either adding a sync-wait primitive to `Async/` -- which
/// is a deadlock generator the first time somebody calls it from a pool or reactor
/// thread, and the tree already has the rule that names the shape -- or
/// hand-writing coroutine frames and a latch here, which is more bespoke
/// concurrency machinery than a pool rather than less.
///
/// So the seam is shaped for the job. The dependency-injection rule asks that the
/// interface be defined and injected; it has never asked that the interface already
/// exist, and every property it protects holds: no hidden pool, substitutable, no
/// ambient global, and the correctness requirement below is testable without a race.
///
/// **One consumer, one home.** If a second appears, that is the moment to ask
/// whether this belongs in `FastCache/` -- not before. A seam moved somewhere
/// shared while it has one caller is a shared thing pretending otherwise.
class IParallelFor
{
  public:
    IParallelFor() = default;
    IParallelFor(IParallelFor const&) = delete;
    IParallelFor(IParallelFor&&) = delete;
    IParallelFor& operator=(IParallelFor const&) = delete;
    IParallelFor& operator=(IParallelFor&&) = delete;
    virtual ~IParallelFor() = default;

    /// Run `slice(i)` for every `i` in `[0, count)` and return once all have ended.
    ///
    /// **A slice that throws is a slice that did not finish, and the caller is told.**
    /// That contract is defined here rather than per implementation, because the
    /// consumer's correctness depends on it: `ProbeToolchainFiles` folds this answer
    /// into `ToolchainFileScan::complete`, and a lost failure there is a truncated
    /// toolchain identity written under a stamp that validates forever -- the same
    /// defect as a short walk, arriving by a different route.
    ///
    /// The exception is deliberately NOT propagated. Rethrowing one of N concurrent
    /// failures means choosing arbitrarily among them and abandoning the rest
    /// mid-flight, and the only question this consumer asks is "did all of it run",
    /// which is exactly a `bool`. Every remaining slice still runs to completion, so
    /// a partial result is a partial result rather than an unknown one.
    ///
    /// `slice` is taken by reference and must NOT be stored: nothing long-lived may
    /// be built from the caller's closure, which is destroyed when `Run` returns.
    ///
    /// @param count Number of slices. Zero is legal and does nothing.
    /// @param slice Called once per index, possibly concurrently, from any thread.
    /// @return True when every slice returned normally; false when at least one threw.
    [[nodiscard]] virtual bool Run(std::size_t count, std::function<void(std::size_t)> const& slice) = 0;
};

} // namespace FastCache::Cc
