// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace FastCache
{

/// Policy interface for capping concurrent connections. The accept loop
/// consults the admission control between accepts; a denied accept means
/// the listener should close the just-accepted socket immediately.
class IAdmissionControl
{
  public:
    IAdmissionControl() = default;
    IAdmissionControl(IAdmissionControl const&) = delete;
    IAdmissionControl(IAdmissionControl&&) = delete;
    IAdmissionControl& operator=(IAdmissionControl const&) = delete;
    IAdmissionControl& operator=(IAdmissionControl&&) = delete;
    virtual ~IAdmissionControl() = default;

    /// @return true if a new connection may be admitted right now.
    [[nodiscard]] virtual bool AllowAccept() noexcept = 0;

    /// Inform the policy that a connection has been admitted (i.e. handed
    /// off to a worker). Counters update so AllowAccept can refuse the
    /// next call once the cap is reached.
    virtual void OnConnectionStarted() noexcept = 0;

    /// Inform the policy that a connection has ended (cleanly or not).
    virtual void OnConnectionEnded() noexcept = 0;
};

/// Default policy: max-concurrent-connections cap. Thread-safe.
class CountingAdmissionControl final: public IAdmissionControl
{
  public:
    /// @param maxConcurrent Connection cap (0 = unlimited).
    explicit CountingAdmissionControl(std::size_t maxConcurrent = 0) noexcept: _max { maxConcurrent } {}

    [[nodiscard]] bool AllowAccept() noexcept override
    {
        if (_max == 0)
            return true;
        return _in.load(std::memory_order_acquire) < _max;
    }

    void OnConnectionStarted() noexcept override { _in.fetch_add(1, std::memory_order_acq_rel); }
    void OnConnectionEnded() noexcept override { _in.fetch_sub(1, std::memory_order_acq_rel); }

    /// @return Current in-flight count.
    [[nodiscard]] std::size_t InFlight() const noexcept { return _in.load(std::memory_order_acquire); }

    /// Adjust the cap at runtime (e.g. from ConfigReloader).
    void SetMax(std::size_t newMax) noexcept { _max = newMax; }

  private:
    std::size_t _max;
    std::atomic<std::size_t> _in { 0 };
};

} // namespace FastCache
