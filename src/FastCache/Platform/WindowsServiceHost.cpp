// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>

#include <atomic>
#include <memory>
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

    void ReportStatus(DWORD state, DWORD waitHintMs = 0)
    {
        currentStatus.dwCurrentState = state;
        currentStatus.dwWin32ExitCode = NO_ERROR;
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

        ReportStatus(SERVICE_STOPPED);
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
