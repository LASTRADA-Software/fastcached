// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Platform/ServiceStatusPlan.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace FastCache
{

/// A control the service manager delivers to a running service.
///
/// Private: never transmitted or persisted. The Win32 implementation maps the SCM's
/// `SERVICE_CONTROL_*` codes onto it, and a code it does not name is `Unsupported`.
enum class ServiceControlRequest : std::uint8_t
{
    Stop,        ///< An operator stopped the service.
    Shutdown,    ///< The machine is shutting down.
    ParamChange, ///< The configuration changed; reload it.
    Interrogate, ///< The manager asks for the current status.
    PowerEvent,  ///< A power broadcast (suspend, resume); its event type names which.
    Unsupported, ///< Anything else, which this service does not handle.
};

/// What handing a thread to the service manager came to.
///
/// Private: never transmitted or persisted.
enum class DispatchOutcome : std::uint8_t
{
    Dispatched,       ///< The manager ran the service main, and it has returned.
    NoServiceManager, ///< This process was not started by a service manager: nothing ran.
    AlreadyRunning,   ///< This process has dispatched once already, which a process may do only once.
};

/// One status report: what the manager is told about the service.
struct ServiceStatusReport
{
    ServiceState state { ServiceState::Stopped }; ///< Where the service is.
    std::uint32_t waitHintMs { 0 };               ///< How long a pending state expects to take.
    ServiceExit exit {};                          ///< Why it stopped; zero unless `Stopped`.
    std::uint32_t checkPoint { 0 };               ///< The stop's progress; zero outside `StopPending`.
    bool acceptsStop { false };                   ///< Whether stop and shutdown are accepted.
    bool acceptsReload { false };                 ///< Whether a parameter change -- a reload -- is accepted.
    bool acceptsPowerEvents { false };            ///< Whether power broadcasts are accepted as well.

    [[nodiscard]] friend bool operator==(ServiceStatusReport const&, ServiceStatusReport const&) = default;
};

/// The three calls a service makes of its manager -- and nothing else.
///
/// **The seam the service host's logic sits above.** Which reports a refused start makes, in
/// what order, and that nothing follows the stop, is platform-neutral and testable; only the
/// calls themselves are Win32 (`StartServiceCtrlDispatcher`, `RegisterServiceCtrlHandlerEx`,
/// `SetServiceStatus`). A host that talked to the SCM directly could be checked only by starting
/// a real service, which no test does, and a refused start once reached the SCM as error 1053
/// because nothing in between could be asserted.
class IServiceControlManager
{
  public:
    /// The service's main function, run by the manager.
    using ServiceMain = std::function<void()>;
    /// A control handler: whether the request was handled. The second argument is the manager's
    /// event type -- a `PBT_*` value for `PowerEvent`, and meaningless for every other request.
    using ControlHandler = std::function<bool(ServiceControlRequest, std::uint32_t eventType)>;

    IServiceControlManager() = default;
    IServiceControlManager(IServiceControlManager const&) = delete;
    IServiceControlManager(IServiceControlManager&&) = delete;
    IServiceControlManager& operator=(IServiceControlManager const&) = delete;
    IServiceControlManager& operator=(IServiceControlManager&&) = delete;
    virtual ~IServiceControlManager() = default;

    /// Hand this thread to the manager, which runs @p main and returns once it has.
    ///
    /// A process may do this once. One the manager did not start has nothing to connect to.
    /// @param name The service's registered name.
    /// @param main What the manager runs.
    /// @return What came of it.
    [[nodiscard]] virtual DispatchOutcome Dispatch(std::string const& name, ServiceMain main) = 0;

    /// Register the control handler: the first thing a service main does, and before it no
    /// status may be reported.
    /// @param name The service's registered name.
    /// @param handler What each control is handed to.
    /// @return Whether the manager accepted it; a service main that cannot register returns.
    [[nodiscard]] virtual bool RegisterHandler(std::string const& name, ControlHandler handler) = 0;

    /// Tell the manager where the service is.
    /// @param report The status.
    virtual void SetStatus(ServiceStatusReport const& report) = 0;
};

} // namespace FastCache
