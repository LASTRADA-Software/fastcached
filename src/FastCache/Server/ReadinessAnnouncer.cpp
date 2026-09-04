// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/ReadinessMarker.hpp>
#include <FastCache/Server/ReadinessAnnouncer.hpp>

#include <utility>

namespace FastCache
{

ReadinessAnnouncer::ReadinessAnnouncer(ILogger& logger, std::string endpointSummary) noexcept:
    _logger { logger },
    _endpointSummary { std::move(endpointSummary) }
{
}

ReadinessAnnouncer::~ReadinessAnnouncer()
{
    if (Announced())
        return;
    // The one state that would otherwise have no line of its own. A waiter blocked
    // on the readiness marker needs the log to say why it never came, and "how many
    // of how many" is what distinguishes a daemon that failed to bind from one whose
    // acceptors were still coming up when it was told to stop.
    _logger.Logf(LogLevel::Warn,
                 "readiness was never announced: {} of {} acceptor(s) armed ({})",
                 ArmedCount(),
                 ExpectedCount(),
                 _endpointSummary);
}

void ReadinessAnnouncer::ExpectAcceptor()
{
    if (_sealed.load(std::memory_order_acquire))
    {
        // A participant created after the set was closed can never be counted, so
        // the readiness line would describe fewer endpoints than are being served.
        // Reported rather than absorbed: this is a wiring mistake in the spawn loop,
        // and it is invisible from the readiness line itself.
        _logger.Log(LogLevel::Warn, "readiness: an acceptor was registered after the set was closed; it is not counted");
        return;
    }
    _expected.fetch_add(1, std::memory_order_acq_rel);
}

void ReadinessAnnouncer::AcceptorsAllSpawned()
{
    _sealed.store(true, std::memory_order_release);
    // The last participant may already have armed while the set was still open, in
    // which case nothing else will ever call in and this is the announcement.
    MaybeAnnounce();
}

void ReadinessAnnouncer::AcceptorArmed(std::string_view what)
{
    auto const armed = _armed.fetch_add(1, std::memory_order_acq_rel) + 1;
    _logger.Logf(LogLevel::Debug, "acceptor armed: {} ({}/{})", what, armed, _expected.load(std::memory_order_acquire));
    MaybeAnnounce();
}

void ReadinessAnnouncer::MaybeAnnounce()
{
    // Nothing may announce before the set is closed, or a participant that arms
    // while its siblings are still being created announces on their behalf.
    if (!_sealed.load(std::memory_order_acquire))
        return;

    auto const expected = _expected.load(std::memory_order_acquire);
    // Zero registered participants is not readiness with none: it is a server with
    // no acceptor, which `RunReactorServer` refuses one step earlier. Being total
    // about it here means that refusal is not load-bearing for this decision.
    if (expected == 0 || _armed.load(std::memory_order_acquire) < expected)
        return;

    // The two callers race deliberately -- the last arm and the seal can each be the
    // one that completes the condition -- so exactly-one is settled here rather than
    // by ordering the calls. A second readiness line would tell a waiter a second
    // daemon had come up.
    if (_announced.test_and_set(std::memory_order_acq_rel))
        return;
    // The marker text comes from `ReadinessMarkerTable`, not from a literal here.
    // Four fixtures in two languages wait on the node's equivalent and none of them
    // is recompiled by this build (#654); the daemon's has two such waiters of its
    // own. A copy in this file would be a second source of truth for a published
    // interface -- the very defect the table exists to close.
    _logger.Logf(
        LogLevel::Info, "{} ({})", ReadinessMarkerText(ReadinessMarker::Daemon), _endpointSummary);
}

bool ReadinessAnnouncer::Announced() const noexcept
{
    return _announced.test(std::memory_order_acquire);
}

std::size_t ReadinessAnnouncer::ArmedCount() const noexcept
{
    return _armed.load(std::memory_order_acquire);
}

std::size_t ReadinessAnnouncer::ExpectedCount() const noexcept
{
    return _expected.load(std::memory_order_acquire);
}

} // namespace FastCache
