// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Transport/AcceptLoopReporter.hpp>

#include <utility>

namespace FastCache
{

AcceptLoopReporter::AcceptLoopReporter(std::string logName,
                                       std::string surface,
                                       ILogger& logger,
                                       core::net::AcceptLoopHealth& health):
    _logName { std::move(logName) },
    _surface { std::move(surface) },
    _logger { logger },
    _health { health }
{
}

AcceptLoopStep AcceptLoopReporter::OnError(core::net::NetError const& error,
                                           core::platform::SteadyTimePoint now,
                                           bool stopping)
{
    using core::net::AcceptAction;
    using core::net::AcceptConditionChange;
    using core::net::AcceptLoopEventKind;

    auto const verdict = _policy.onError(error.code, now);
    if (verdict.action != AcceptAction::AcceptAgain && stopping)
    {
        // The owner is stopping, so an accept that ends the loop is the stop arriving, whichever way
        // the platform words it: a close answers `Cancelled`, and on Windows an acceptor that calls
        // `AcceptRaw` on a handle the teardown has just closed answers WSAENOTSOCK, which classifies
        // as a dead handle. Neither is a surface that gave up, and neither is this loop's to close --
        // the owner is closing it. Reporting it would write an Error line and latch
        // `surface-not-accepting` on an orderly stop.
        OnLoopEnded();
        return AcceptLoopStep { .delay = {}, .next = AcceptLoopNext::End };
    }
    switch (verdict.action)
    {
        case AcceptAction::Stop:
            // Closed under a loop that was meant to serve: the surface no longer accepts, and that
            // is exactly what the registry is for. Already closed, so there is nothing to close.
            Report(AcceptLoopEventKind::GaveUp, LogLevel::Error, core::net::describeAcceptLoopEnded(_logName, error), error);
            return AcceptLoopStep { .delay = {}, .next = AcceptLoopNext::End };
        case AcceptAction::GiveUp:
            // A dead listener left open goes on queueing handshakes nobody accepts: the loop closes
            // it, so the port refuses them, and the end is reported -- nobody was stopping, so a dead
            // handle here is no shutdown.
            Report(AcceptLoopEventKind::GaveUp, LogLevel::Error, core::net::describeAcceptLoopEnded(_logName, error), error);
            return AcceptLoopStep { .delay = {}, .next = AcceptLoopNext::EndAndClose };
        case AcceptAction::AcceptAgain:
            break;
    }

    if (verdict.change == AcceptConditionChange::Degraded)
        Report(AcceptLoopEventKind::Degraded,
               LogLevel::Error,
               core::net::describeAcceptDegraded(_logName, error, verdict.streak),
               error);
    else if (verdict.change == AcceptConditionChange::Recovered)
        Report(AcceptLoopEventKind::Recovered,
               LogLevel::Info,
               core::net::describeAcceptRecovered(_logName, verdict.streak),
               core::net::NetError {});
    if (verdict.warning.has_value())
        // Rate-limited by the policy; a warning changes no condition, so it reaches the log alone.
        _logger.Log(LogLevel::Warn, core::net::describeAcceptFailure(_logName, error, verdict));
    return AcceptLoopStep { .delay = verdict.delay, .next = AcceptLoopNext::AcceptAgain };
}

void AcceptLoopReporter::OnAccepted(core::platform::SteadyTimePoint now)
{
    auto const verdict = _policy.onAccepted(now);
    if (verdict.change == core::net::AcceptConditionChange::Recovered)
        Report(core::net::AcceptLoopEventKind::Recovered,
               LogLevel::Info,
               core::net::describeAcceptRecovered(_logName, verdict.streak),
               core::net::NetError {});
}

void AcceptLoopReporter::OnLoopEnded()
{
    if (_ended || !_policy.degraded())
        return;
    Report(core::net::AcceptLoopEventKind::Stopped,
           LogLevel::Info,
           core::net::describeAcceptLoopStopped(_logName),
           core::net::NetError {});
}

void AcceptLoopReporter::Report(core::net::AcceptLoopEventKind kind,
                                LogLevel level,
                                std::string line,
                                core::net::NetError error)
{
    if (kind == core::net::AcceptLoopEventKind::GaveUp || kind == core::net::AcceptLoopEventKind::Stopped)
        _ended = true;
    _logger.Log(level, line);
    _health.record(core::net::AcceptLoopEvent {
        .surface = _surface, .line = std::move(line), .error = std::move(error), .kind = kind });
}

core::async::Task<void> WaitOutBackoff(core::net::EventLoop* reactor, IDrainWait* blocking, std::chrono::milliseconds delay)
{
    if (delay <= std::chrono::milliseconds {})
        co_return;
    if (reactor != nullptr)
        co_await reactor->delay(delay);
    else
        blocking->Sleep(delay);
    co_return;
}

} // namespace FastCache
