// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/IServiceControlManager.hpp>
#include <FastCache/Platform/ServiceHost.hpp>
#include <FastCache/Platform/ServiceStatusPlan.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

#if !defined(_WIN32)

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& /*serviceName*/, ServiceHostOptions /*options*/)
{
    return nullptr; // unsupported on non-Windows
}

#else

namespace
{

    /// The service main `Dispatch` hands the SCM. `SERVICE_MAIN_FUNCTION` carries no context, and
    /// the dispatcher is process-wide, so at most one is pending per process.
    IServiceControlManager::ServiceMain const* pendingServiceMain { nullptr };

    /// The control handler the SCM calls, kept for the life of the PROCESS and never destroyed.
    ///
    /// The SCM keeps a registration that long and calls it on a thread of its own, while the
    /// manager object that made it is freed with the host as soon as the dispatcher returns. A
    /// handler stored in the manager -- the SCM's context pointer -- was therefore read in freed
    /// memory by a control that was already on its way. What is kept here is `ServiceHost`'s gate
    /// (`ServiceControlGate`), which reaches the host only while it is there. Never destroyed
    /// rather than static, because static destruction runs at exit while the SCM's thread can
    /// still be inside a call. One registration per process, as there is one dispatch.
    /// @return The process's registration slot.
    [[nodiscard]] IServiceControlManager::ControlHandler& RegisteredControlHandler()
    {
        using Handler = IServiceControlManager::ControlHandler;
        alignas(Handler) static std::array<std::byte, sizeof(Handler)> storage {};
        static Handler* const handler = std::construct_at(reinterpret_cast<Handler*>(storage.data()));
        return *handler;
    }

    /// One SCM control code, and what it means here.
    struct ControlRow
    {
        DWORD code;                    ///< `SERVICE_CONTROL_*`.
        ServiceControlRequest request; ///< What the host is handed.
    };

    constexpr auto ControlRows = std::to_array<ControlRow>({
        { .code = SERVICE_CONTROL_STOP, .request = ServiceControlRequest::Stop },
        { .code = SERVICE_CONTROL_SHUTDOWN, .request = ServiceControlRequest::Shutdown },
        { .code = SERVICE_CONTROL_PARAMCHANGE, .request = ServiceControlRequest::ParamChange },
        { .code = SERVICE_CONTROL_INTERROGATE, .request = ServiceControlRequest::Interrogate },
        { .code = SERVICE_CONTROL_POWEREVENT, .request = ServiceControlRequest::PowerEvent },
    });

    /// One reported state, and its Win32 constant.
    struct StateRow
    {
        ServiceState state; ///< As the plan names it.
        DWORD code;         ///< What `dwCurrentState` carries.
    };

    constexpr auto StateRows = std::to_array<StateRow>({
        { .state = ServiceState::StartPending, .code = SERVICE_START_PENDING },
        { .state = ServiceState::Running, .code = SERVICE_RUNNING },
        { .state = ServiceState::StopPending, .code = SERVICE_STOP_PENDING },
        { .state = ServiceState::Stopped, .code = SERVICE_STOPPED },
    });

    /// The SCM's three calls and nothing else; `ServiceHost` decides what to say through them.
    class Win32ServiceControlManager final: public IServiceControlManager
    {
      public:
        [[nodiscard]] DispatchOutcome Dispatch(std::string const& name, ServiceMain main) override
        {
            pendingServiceMain = &main;
            // SERVICE_TABLE_ENTRYA takes a mutable char*; the SCM does not modify the name but the
            // API signature requires non-const.
            auto mutableName = name;
            std::array table {
                SERVICE_TABLE_ENTRYA { .lpServiceName = mutableName.data(), .lpServiceProc = &RunPendingServiceMain },
                SERVICE_TABLE_ENTRYA { .lpServiceName = nullptr, .lpServiceProc = nullptr },
            };
            auto const dispatched = StartServiceCtrlDispatcherA(table.data()) != 0;
            auto const error = dispatched ? DWORD { ERROR_SUCCESS } : GetLastError();
            pendingServiceMain = nullptr;
            if (dispatched)
                return DispatchOutcome::Dispatched;
            return error == ERROR_SERVICE_ALREADY_RUNNING ? DispatchOutcome::AlreadyRunning
                                                          : DispatchOutcome::NoServiceManager;
        }

        [[nodiscard]] bool RegisterHandler(std::string const& name, ControlHandler handler) override
        {
            // Stored before the SCM can call it, and nowhere this object's lifetime reaches.
            RegisteredControlHandler() = std::move(handler);
            _status = RegisterServiceCtrlHandlerExA(name.c_str(), &HandleControl, nullptr);
            return _status != nullptr;
        }

        void SetStatus(ServiceStatusReport const& report) override
        {
            static_assert(ServiceSpecificError == ERROR_SERVICE_SPECIFIC_ERROR,
                          "ServiceStatusPlan.hpp spells ERROR_SERVICE_SPECIFIC_ERROR without <windows.h>");
            SERVICE_STATUS status {};
            status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
            status.dwCurrentState = SERVICE_STOPPED;
            for (auto const& row: StateRows)
                if (row.state == report.state)
                    status.dwCurrentState = row.code;
            status.dwControlsAccepted =
                (report.acceptsControls
                     ? DWORD { SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PARAMCHANGE }
                     : DWORD { 0 })
                | (report.acceptsPowerEvents ? DWORD { SERVICE_ACCEPT_POWEREVENT } : DWORD { 0 });
            status.dwWin32ExitCode = report.exit.win32ExitCode;
            status.dwServiceSpecificExitCode = report.exit.serviceSpecificExitCode;
            status.dwWaitHint = report.waitHintMs;
            status.dwCheckPoint = report.checkPoint;
            if (_status != nullptr)
                SetServiceStatus(_status, &status);
        }

      private:
        /// The SCM's entry: the service main `Dispatch` is waiting on.
        static void WINAPI RunPendingServiceMain(DWORD /*argc*/, LPSTR* /*argv*/)
        {
            if (pendingServiceMain != nullptr && *pendingServiceMain)
                (*pendingServiceMain)();
        }

        /// The SCM's control handler, which it calls on a thread of its own. It reads nothing of
        /// this object -- which may already be gone -- only the process's registration.
        static DWORD WINAPI HandleControl(DWORD control, DWORD eventType, LPVOID /*eventData*/, LPVOID /*context*/)
        {
            auto request = ServiceControlRequest::Unsupported;
            for (auto const& row: ControlRows)
                if (row.code == control)
                    request = row.request;
            auto const& handler = RegisteredControlHandler();
            return handler && handler(request, eventType) ? DWORD { NO_ERROR } : DWORD { ERROR_CALL_NOT_IMPLEMENTED };
        }

        SERVICE_STATUS_HANDLE _status { nullptr };
    };

} // namespace

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& serviceName, ServiceHostOptions options)
{
    return std::make_unique<ServiceHost>(serviceName,
                                         std::make_unique<Win32ServiceControlManager>(),
                                         DaemonControls::Instance(),
                                         options,
                                         DefaultDrainWait());
}

#endif // _WIN32

} // namespace FastCache
