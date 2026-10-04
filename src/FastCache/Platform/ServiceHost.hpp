// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/IServiceControlManager.hpp>
#include <FastCache/Platform/ServiceStatusPlan.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace FastCache
{

/// What `ServiceHost::Run` exits with when there was no service manager to run the body under.
inline constexpr int ServiceNotDispatchedExit = 1;

/// What stands between a service manager's control thread and the host it delivers to.
///
/// The manager keeps a control-handler registration for the life of the PROCESS, and calls it on a
/// thread of its own; `main` frees the host as soon as the dispatcher returns. So the registration
/// holds this gate rather than the host, the gate holds the host only while it is open, and a
/// delivery runs WHOLLY under the gate's lock: `Close` waits out a delivery in flight, and every
/// delivery after it answers false and reaches nothing.
class ServiceControlGate
{
  public:
    /// @param deliver What an open gate hands a control to.
    explicit ServiceControlGate(std::function<bool(ServiceControlRequest, std::uint32_t)> deliver) noexcept;

    /// Hand @p request to the host, if it is still there.
    /// @param request The control.
    /// @param eventType The manager's event type: a `PBT_*` value for a power event.
    /// @return What the host answered; false once the gate is closed.
    [[nodiscard]] bool Deliver(ServiceControlRequest request, std::uint32_t eventType);

    /// Reach the host no more, once any delivery in flight has returned.
    void Close();

  private:
    std::mutex _mutex;
    std::function<bool(ServiceControlRequest, std::uint32_t)> _deliver; ///< Guarded by `_mutex`; empty once closed.
};

/// The daemon host a service manager runs: platform-neutral, over `IServiceControlManager`.
///
/// A start walks its row of `ServiceHostStartTable` -- a serving one reports starting and running
/// before the body, a refused one reports nothing before the stop -- and every run ends with one
/// `Stopped` report carrying the exit code (`ServiceExitFor`), after which nothing is reported:
/// a stop request racing the body's return must not tell the manager `StopPending` after it was
/// told the service stopped.
///
/// **A stop reports its progress while the body winds down** (`ServiceHostOptions::stop`): the
/// first stop or shutdown control reports `StopPending` with the plan's wait hint and starts a
/// reporter that advances the checkpoint every `checkpointEvery` until the body returns
/// (`ReportStopProgress`), so a drain longer than one hint does not read to the manager as a hung
/// service. A second stop control changes nothing -- reporting the initial `StopPending` again
/// would reset the checkpoint mid-drain, which reads as the stop starting over. And a service
/// handed a host-event sink accepts power broadcasts while it runs, handing each the sink as a
/// `HostEvent` (`HostEventForPowerBroadcast`).
class ServiceHost final: public IDaemonHost
{
  public:
    /// @param name The service's registered name.
    /// @param manager The service manager's three calls.
    /// @param controls Where a stop or reload request is raised for the body to see.
    /// @param options How a stop is reported, and where power events go.
    /// @param stopWait Where the reporter spends the gap between two checkpoints; must outlive this.
    ServiceHost(std::string name,
                std::unique_ptr<IServiceControlManager> manager,
                DaemonControls& controls,
                ServiceHostOptions options,
                IDrainWait& stopWait);

    /// Closes the control gate before any member goes, so a control the manager is delivering
    /// finishes first and none after it reaches this object.
    ~ServiceHost() override;

    ServiceHost(ServiceHost const&) = delete;
    ServiceHost& operator=(ServiceHost const&) = delete;
    ServiceHost(ServiceHost&&) = delete;
    ServiceHost& operator=(ServiceHost&&) = delete;

    /// @copydoc IDaemonHost::Run
    [[nodiscard]] int Run(Body body) override;

    /// @copydoc IDaemonHost::Refuse
    [[nodiscard]] int Refuse(int exitCode) override;

  private:
    /// The service main for @p start: register, report the row, run @p body if there is one, stop.
    void ServiceMainFor(ServiceHostStart start, Body const& body);

    /// Answer one control the manager delivers.
    [[nodiscard]] bool HandleControl(ServiceControlRequest request, std::uint32_t eventType);

    /// Report @p state, unless the stop has been reported already.
    void Report(ServiceState state, std::uint32_t waitHintMs, int exitCode, std::uint32_t checkPoint = 0);

    /// Advance the start's checkpoint until the body serves or returns, within the plan's ceiling,
    /// and report RUNNING if it serves first. Runs on `_startReporter`.
    void ReportStartUntilServing();

    std::string _name;
    std::unique_ptr<IServiceControlManager> _manager;
    DaemonControls& _controls;
    ServiceHostOptions _options;
    IDrainWait& _stopWait;
    std::atomic<int> _exitCode { 0 };
    /// Serializes the reports: the manager calls the handler on a thread of its own while the
    /// service main runs the body on another, and the stop reporter reports from a third.
    std::mutex _statusMutex;
    bool _stopReported { false }; ///< Guarded by `_statusMutex`.
    /// What the manager's control-handler registration holds instead of this object.
    std::shared_ptr<ServiceControlGate> _gate;
    /// Set once the body has returned, which is what ends the stop reporter.
    std::atomic<bool> _bodyReturned { false };
    /// Advances the start checkpoint until the body serves or returns; started by a start whose row
    /// awaits serving, joined by the service main before the stop.
    std::jthread _startReporter;
    /// Guards `_stopReporter`, which the control handler starts and the service main joins.
    std::mutex _reporterMutex;
    /// Advances the stop checkpoint until the body returns; started by the first stop control.
    /// Declared LAST: its body touches the members above, so it is joined before they go.
    std::jthread _stopReporter;
};

} // namespace FastCache
