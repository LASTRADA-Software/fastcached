// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/ServiceHost.hpp>
#include <FastCache/Platform/StopPending.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <tuple>
#include <utility>

namespace FastCache
{

namespace
{
    /// @p hint as the manager's `DWORD` milliseconds; `StopPendingPlanFor` already clamps a plan to
    /// `MaxServiceWaitHint`, so nothing narrowed here wraps.
    [[nodiscard]] std::uint32_t WaitHintMs(std::chrono::milliseconds hint) noexcept
    {
        return static_cast<std::uint32_t>(std::min(hint, MaxServiceWaitHint).count());
    }
} // namespace

ServiceControlGate::ServiceControlGate(std::function<bool(ServiceControlRequest, std::uint32_t)> deliver) noexcept:
    _deliver { std::move(deliver) }
{
}

bool ServiceControlGate::Deliver(ServiceControlRequest request, std::uint32_t eventType)
{
    std::scoped_lock const guard { _mutex };
    return _deliver && _deliver(request, eventType);
}

void ServiceControlGate::Close()
{
    std::scoped_lock const guard { _mutex };
    _deliver = nullptr;
}

ServiceHost::ServiceHost(std::string name,
                         std::unique_ptr<IServiceControlManager> manager,
                         DaemonControls& controls,
                         ServiceHostOptions options,
                         IDrainWait& stopWait):
    _name { std::move(name) },
    _manager { std::move(manager) },
    _controls { controls },
    _options { options },
    _stopWait { stopWait },
    _gate { std::make_shared<ServiceControlGate>(
        [this](ServiceControlRequest request, std::uint32_t eventType) { return HandleControl(request, eventType); }) }
{
}

ServiceHost::~ServiceHost()
{
    _gate->Close();
}

int ServiceHost::Run(Body body)
{
    auto const start = ServiceReadinessTable[static_cast<std::size_t>(_options.readiness)].start;
    auto const outcome = _manager->Dispatch(_name, [this, start, &body] { ServiceMainFor(start, body); });
    if (outcome != DispatchOutcome::Dispatched)
        return ServiceNotDispatchedExit;
    return _exitCode.load(std::memory_order_acquire);
}

int ServiceHost::Refuse(int exitCode)
{
    // No body: the run reports the stop and nothing else. Whether the dispatcher connects does not
    // change the answer -- started by hand with `--daemon` there is no manager to tell, and the
    // exit code is then the whole report.
    _exitCode.store(exitCode, std::memory_order_release);
    std::ignore = _manager->Dispatch(_name, [this] { ServiceMainFor(ServiceHostStart::Refused, Body {}); });
    return exitCode;
}

void ServiceHost::ServiceMainFor(ServiceHostStart start, Body const& body)
{
    // The GATE, never `this`: the manager keeps this registration for the life of the process.
    if (!_manager->RegisterHandler(_name, [gate = _gate](ServiceControlRequest request, std::uint32_t eventType) {
            return gate->Deliver(request, eventType);
        }))
        return;
    auto const& row = ServiceHostStartTable[static_cast<std::size_t>(start)];
    for (auto const& report: row.beforeStop)
        Report(report.state, report.waitHintMs, 0);
    if (row.awaitsServing)
        _startReporter = std::jthread { [this] { ReportStartUntilServing(); } };

    if (body)
        _exitCode.store(body(), std::memory_order_release);

    // Both reporters end on the body's return, and are joined BEFORE the stop: a checkpoint either
    // reported after `Stopped` would be the out-of-order report `Report` refuses anyway, but one
    // racing the stop would leave the manager's last word to chance.
    _bodyReturned.store(true, std::memory_order_release);
    if (_startReporter.joinable())
        _startReporter.join();
    {
        std::scoped_lock const guard { _reporterMutex };
        if (_stopReporter.joinable())
            _stopReporter.join();
    }
    Report(ServiceState::Stopped, 0, _exitCode.load(std::memory_order_acquire));
}

bool ServiceHost::HandleControl(ServiceControlRequest request, std::uint32_t eventType)
{
    switch (request)
    {
        case ServiceControlRequest::Stop:
        case ServiceControlRequest::Shutdown: {
            _controls.RequestStop();
            std::scoped_lock const guard { _reporterMutex };
            // A second stop control -- `sc stop` followed by a system shutdown, say -- must change
            // nothing: reporting the initial `StopPending` again would reset the checkpoint to zero
            // mid-drain, which reads to the manager as the stop having just started over. Whether
            // the reporter runs is the one "already stopping" fact, rather than a second flag that
            // could disagree with it.
            // Nor does a stop that arrives after the body returned, which has nothing left to report.
            if (!_stopReporter.joinable() && !_bodyReturned.load(std::memory_order_acquire))
            {
                Report(ServiceState::StopPending, WaitHintMs(_options.stop.waitHint), 0);
                _stopReporter = std::jthread { [this] {
                    std::ignore = ReportStopProgress(
                        _options.stop,
                        [this] { return _bodyReturned.load(std::memory_order_acquire); },
                        [this](std::uint32_t checkPoint, std::chrono::milliseconds hint) {
                            Report(ServiceState::StopPending, WaitHintMs(hint), 0, checkPoint);
                        },
                        _stopWait);
                } };
            }
            return true;
        }
        case ServiceControlRequest::ParamChange:
            _controls.RequestReload();
            return true;
        case ServiceControlRequest::PowerEvent:
            // Synchronous: the sink runs on the manager's thread, and Windows allows about two
            // seconds for a suspend notification, so whatever a suspend's sink says before the
            // machine sleeps is bounded well inside that. Accepted only while running, so none
            // arrives once the body is winding down.
            if (_options.hostEvents == nullptr)
                return false;
            if (auto const event = HostEventForPowerBroadcast(eventType))
                _options.hostEvents->OnHostEvent(*event);
            return true;
        case ServiceControlRequest::Interrogate:
            return true;
        case ServiceControlRequest::Unsupported:
            break;
    }
    return false;
}

void ServiceHost::ReportStartUntilServing()
{
    auto const served = [this] {
        return _controls.Serving();
    };
    auto const returned = [this] {
        return _bodyReturned.load(std::memory_order_acquire);
    };
    // The ceiling is MEASURED on the wait's own clock rather than counted in checkpoints: a sleep
    // costs what the host's timer grants, not what was asked (`DrainWithin`'s reason).
    auto const began = _stopWait.Now();
    auto const pastCeiling = [this, began] {
        return _stopWait.Now() - began >= _options.start.ceiling;
    };
    std::ignore = ReportStopProgress(
        StopPendingPlan { .waitHint = _options.start.waitHint, .checkpointEvery = _options.start.checkpointEvery },
        [&] { return served() || returned() || pastCeiling(); },
        [this](std::uint32_t checkPoint, std::chrono::milliseconds hint) {
            Report(ServiceState::StartPending, WaitHintMs(hint), 0, checkPoint);
        },
        _stopWait);
    // Past the ceiling the checkpoint stands still, so whoever waits on the start gives up -- but the
    // start goes on, and a body that serves late is still reported RUNNING, or it could never be stopped.
    while (!served() && !returned())
        _stopWait.Sleep(_options.start.checkpointEvery);
    if (served() && !returned())
        Report(ServiceState::Running, 0, 0);
}

void ServiceHost::Report(ServiceState state, std::uint32_t waitHintMs, int exitCode, std::uint32_t checkPoint)
{
    auto const report = ServiceStatusReport {
        .state = state,
        .waitHintMs = waitHintMs,
        .exit = ServiceExitFor(state == ServiceState::Stopped ? exitCode : 0),
        .checkPoint = checkPoint,
        .acceptsControls = state == ServiceState::Running,
        .acceptsPowerEvents = state == ServiceState::Running && _options.hostEvents != nullptr,
    };
    if (state != ServiceState::Stopped)
    {
        // Reported UNDER the lock, so the stop below cannot overtake it.
        std::scoped_lock const guard { _statusMutex };
        if (!_stopReported)
            _manager->SetStatus(report);
        return;
    }

    // The stop is decided under the lock and reported after it, as the LAST thing this host does:
    // once the SCM records STOPPED its dispatcher returns on the main thread and `main` destroys
    // this object, so a lock released after the report would be released in freed memory. Taking
    // the lock waits out a report in flight; setting the flag keeps every later one from starting.
    auto* const manager = _manager.get();
    {
        std::scoped_lock const guard { _statusMutex };
        if (_stopReported)
            return;
        _stopReported = true;
    }
    manager->SetStatus(report);
}

} // namespace FastCache
