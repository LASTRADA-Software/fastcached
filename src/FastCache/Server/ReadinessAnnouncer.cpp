// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Server/ReadinessAnnouncer.hpp>

#include <utility>

namespace FastCache
{

ReadinessAnnouncer::ReadinessAnnouncer(ILogger& logger, std::size_t acceptorCount, std::string endpointSummary) noexcept:
    _logger { logger },
    _acceptorCount { acceptorCount },
    _endpointSummary { std::move(endpointSummary) }
{
}

void ReadinessAnnouncer::AcceptorArmed(std::string_view what)
{
    // Reading the value this call produced, not the counter afterwards: two threads
    // arming at once must each see their own position, or both could read the total
    // and both announce.
    auto const armed = _armed.fetch_add(1, std::memory_order_acq_rel) + 1;
    _logger.Logf(LogLevel::Debug, "acceptor armed: {} ({}/{})", what, armed, _acceptorCount);

    // Equality rather than `>=`, so the transition is observed by exactly one caller.
    // An overshoot means the caller mis-declared how many acceptors it would arm,
    // which is a wiring defect worth a line -- and it must not be allowed to announce
    // a second time, because a fixture that has already been told the daemon is ready
    // reads a second line as a second daemon.
    if (armed == _acceptorCount)
        _logger.Logf(LogLevel::Info, "ready, accepting connections ({})", _endpointSummary);
    else if (armed > _acceptorCount)
        _logger.Logf(LogLevel::Warn,
                     "readiness: {} acceptors armed but only {} were declared; the readiness line under-reports",
                     armed,
                     _acceptorCount);
}

bool ReadinessAnnouncer::Announced() const noexcept
{
    // A zero-acceptor announcer never announces, so it must not report that it has.
    return _acceptorCount != 0 && _armed.load(std::memory_order_acquire) >= _acceptorCount;
}

std::size_t ReadinessAnnouncer::ArmedCount() const noexcept
{
    return _armed.load(std::memory_order_acquire);
}

} // namespace FastCache
