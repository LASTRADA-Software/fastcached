// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

#if !defined(_WIN32)

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& /*serviceName*/)
{
    return nullptr; // unsupported on non-Windows
}

#else

namespace
{

    /// Globals used by the SCM bridge. The dispatcher is process-wide, so
    /// at most one Windows service body runs per process.
    SERVICE_STATUS_HANDLE serviceStatus { nullptr };
    SERVICE_STATUS currentStatus {};
    IDaemonHost::Body serviceBody;
    std::atomic<int> exitCode { 0 };
    /// Service name the SCM dispatcher was started with; used by ServiceMain so
    /// a custom --service-name matches what was registered at install time.
    std::string registeredServiceName { "FastCached" };

    /// Guards `currentStatus` and the `SetServiceStatus` that publishes it.
    ///
    /// Two threads report: the SCM calls the control handler on one of its own while
    /// `ServiceMain` runs the body on another. Unsynchronized, a STOP arriving as the
    /// body returns can interleave so that the last thing the SCM is told is
    /// STOP_PENDING with no exit code -- and the failure actions, which only fire on
    /// a service that reported failure, never see one. That was harmless while every
    /// report said `NO_ERROR`; it stopped being harmless the moment one of them
    /// carried the reason.
    std::mutex statusMutex;

    /// Tell the SCM where the service is, and -- when it has stopped -- whether it
    /// stopped because it failed.
    ///
    /// The exit code half is what makes the failure actions set at registration
    /// reachable at all. This reported `NO_ERROR` unconditionally, so a body that
    /// returned a failure code told the SCM it had stopped cleanly, and a policy that
    /// restarts a FAILED service therefore never saw one. A configured policy that
    /// cannot fire is worse than none, because `sc qfailure` shows it and an operator
    /// believes it.
    ///
    /// An application's own code is reported the way this API asks for it: an
    /// out-of-band `dwWin32ExitCode` saying "the specific code is in the other
    /// field", rather than a Win32 error number this process never had. Zero stays
    /// `NO_ERROR`, so a clean stop still reads as one and nothing restarts a service
    /// an operator asked to stop.
    /// @param state The SCM state to report.
    /// @param waitHintMs How long a pending transition expects to take.
    /// @param bodyExitCode The daemon body's exit code; only read for SERVICE_STOPPED.
    void ReportStatus(DWORD state, DWORD waitHintMs = 0, int bodyExitCode = 0)
    {
        std::scoped_lock const guard { statusMutex };

        currentStatus.dwCurrentState = state;
        if (state == SERVICE_STOPPED && bodyExitCode != 0)
        {
            currentStatus.dwWin32ExitCode = ERROR_SERVICE_SPECIFIC_ERROR;
            currentStatus.dwServiceSpecificExitCode = static_cast<DWORD>(bodyExitCode);
        }
        else
        {
            currentStatus.dwWin32ExitCode = NO_ERROR;
            currentStatus.dwServiceSpecificExitCode = 0;
        }
        currentStatus.dwWaitHint = waitHintMs;
        if (state == SERVICE_RUNNING)
            currentStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PARAMCHANGE;
        else
            currentStatus.dwControlsAccepted = 0;
        if (serviceStatus)
            SetServiceStatus(serviceStatus, &currentStatus);
    }

    DWORD WINAPI ServiceCtrlHandlerEx(DWORD ctrl, DWORD /*evt*/, LPVOID /*evtData*/, LPVOID /*ctx*/)
    {
        switch (ctrl)
        {
            case SERVICE_CONTROL_STOP:
            case SERVICE_CONTROL_SHUTDOWN:
                ReportStatus(SERVICE_STOP_PENDING, 10000);
                DaemonControls::Instance().RequestStop();
                return NO_ERROR;
            case SERVICE_CONTROL_PARAMCHANGE:
                DaemonControls::Instance().RequestReload();
                return NO_ERROR;
            case SERVICE_CONTROL_INTERROGATE:
                return NO_ERROR;
            default:
                return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    void WINAPI ServiceMain(DWORD /*argc*/, LPSTR* /*argv*/)
    {
        serviceStatus = RegisterServiceCtrlHandlerExA(registeredServiceName.c_str(), &ServiceCtrlHandlerEx, nullptr);
        if (!serviceStatus)
            return;
        currentStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ReportStatus(SERVICE_START_PENDING, 5000);
        ReportStatus(SERVICE_RUNNING);

        if (serviceBody)
            exitCode.store(serviceBody(), std::memory_order_release);

        ReportStatus(SERVICE_STOPPED, 0, exitCode.load(std::memory_order_acquire));
    }

    class WindowsServiceHost final: public IDaemonHost
    {
      public:
        explicit WindowsServiceHost(std::string name) noexcept:
            _name { std::move(name) }
        {
        }

        int Run(Body body) override
        {
            serviceBody = std::move(body);
            registeredServiceName = _name;
            // SERVICE_TABLE_ENTRYA takes a mutable char*; the SCM does not
            // modify the name but the API signature requires non-const.
            SERVICE_TABLE_ENTRYA table[] = {
                { _name.data(), &ServiceMain },
                { nullptr, nullptr },
            };
            if (!StartServiceCtrlDispatcherA(table))
                return 1;
            return exitCode.load(std::memory_order_acquire);
        }

      private:
        std::string _name;
    };

} // namespace

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string const& serviceName)
{
    return std::make_unique<WindowsServiceHost>(serviceName);
}

#endif // _WIN32

} // namespace FastCache
