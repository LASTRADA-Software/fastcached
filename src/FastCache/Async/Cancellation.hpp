// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <memory>

namespace FastCache
{

namespace Detail
{

    /// Shared state between a cancellation source and the tokens it issues.
    /// A single flag is enough for the current usage pattern — we don't
    /// expose callback registration yet; consumers poll IsCancelled() at
    /// each suspend point.
    struct CancellationState
    {
        std::atomic<bool> cancelled { false };
    };

} // namespace Detail

/// Token observed by tasks/awaitables to learn that cancellation has been
/// requested. Cheap to copy; multiple tokens may share one source. Equivalent
/// to std::stop_token in std::jthread land, but without the callback
/// dispatcher (which we don't need yet).
class CancellationToken
{
  public:
    CancellationToken() = default;

    /// @return true if the linked source has been cancelled. A
    /// default-constructed token always returns false.
    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return _state && _state->cancelled.load(std::memory_order_acquire);
    }

  private:
    friend class CancellationSource;

    explicit CancellationToken(std::shared_ptr<Detail::CancellationState> state) noexcept:
        _state { std::move(state) }
    {
    }

    std::shared_ptr<Detail::CancellationState> _state;
};

/// Source half: writing side of the cancellation pair. Calling Cancel() is
/// observable through every token that was minted from this source.
class CancellationSource
{
  public:
    CancellationSource():
        _state { std::make_shared<Detail::CancellationState>() }
    {
    }

    /// @return A token that observes this source.
    [[nodiscard]] CancellationToken Token() const noexcept
    {
        return CancellationToken { _state };
    }

    /// @return true if Cancel() has already been called.
    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return _state->cancelled.load(std::memory_order_acquire);
    }

    /// Request cancellation. Idempotent.
    void Cancel() noexcept
    {
        _state->cancelled.store(true, std::memory_order_release);
    }

  private:
    std::shared_ptr<Detail::CancellationState> _state;
};

} // namespace FastCache
