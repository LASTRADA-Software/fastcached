// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "IParallelFor.hpp"

#include <cstddef>
#include <functional>

namespace FastCache::Cc
{

/// Runs every slice on the calling thread, in order.
///
/// The production implementation on a machine with one core, and the one tests
/// use: a walk driven through this is deterministic, so the properties that
/// matter -- that a failing slice clears `complete`, that the digest does not
/// depend on ordering -- are asserted without arranging a race.
class SerialParallelFor final: public IParallelFor
{
  public:
    [[nodiscard]] bool Run(std::size_t count, std::function<void(std::size_t)> const& slice) override;
};

/// How many slices `ThreadedParallelFor` runs at once by default.
///
/// Derived rather than a literal, and it keys on `hardware_concurrency` with a
/// DELIBERATE oversubscription, because the work this exists for is not CPU work.
/// The cost is per-file open latency -- measured on Windows at 5.00 ms/file cold
/// against 0.21 ms warm, so roughly ninety-five percent of it is waiting on the
/// filesystem and on whatever inspects each open. Sizing to core count would leave
/// most of that latency unoverlapped, which is the whole win.
///
/// Clamped at both ends: a floor so a single-core CI runner still overlaps waits,
/// and a ceiling because past a point the queue is at the disk rather than at us,
/// and every extra thread is a stack and a scheduling cost for nothing.
///
/// @return The default width, at least 4 and at most 32.
[[nodiscard]] std::size_t DefaultParallelWidth();

/// Runs slices across a bounded set of threads, joining before it returns.
///
/// Threads are created per `Run` rather than kept: this is called once per
/// toolchain at startup, so a resident pool would be machinery owned for the life
/// of the process to serve an operation that happens twice.
class ThreadedParallelFor final: public IParallelFor
{
  public:
    /// @param width How many slices to run at once; clamped to at least one.
    explicit ThreadedParallelFor(std::size_t width = DefaultParallelWidth());

    [[nodiscard]] bool Run(std::size_t count, std::function<void(std::size_t)> const& slice) override;

    /// @return The configured width.
    [[nodiscard]] std::size_t Width() const noexcept
    {
        return _width;
    }

  private:
    std::size_t _width;
};

} // namespace FastCache::Cc
