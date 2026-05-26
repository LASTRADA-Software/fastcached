// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/IDaemonHost.hpp>

#include <FastCache/Platform/DaemonControls.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

#if !defined(_WIN32)

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string /*serviceName*/)
{
    return nullptr; // unsupported on non-Windows
}

#else

namespace
{

    /// Globals used by the SCM bridge. The dispatcher is process-wide, so
    /// at most one Windows service body runs per process.
    SERVICE_STATUS_HANDLE g_serviceStatus { nullptr };
    SERVICE_STATUS g_currentStatus {};
    IDaemonHost::Body g_body;
    std::atomic<int> g_exitCode { 0 };

    void ReportStatus(DWORD state, DWORD waitHintMs = 0)
    {
        g_currentStatus.dwCurrentState = state;
        g_currentStatus.dwWin32ExitCode = NO_ERROR;
        g_currentStatus.dwWaitHint = waitHintMs;
        if (state == SERVICE_RUNNING)
            g_currentStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
                | SERVICE_ACCEPT_PARAMCHANGE;
        else
            g_currentStatus.dwControlsAccepted = 0;
        if (g_serviceStatus)
            SetServiceStatus(g_serviceStatus, &g_currentStatus);
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
            case SERVICE_CONTROL_INTERROGATE: return NO_ERROR;
            default:                          return ERROR_CALL_NOT_IMPLEMENTED;
        }
    }

    void WINAPI ServiceMain(DWORD /*argc*/, LPSTR* /*argv*/)
    {
        g_serviceStatus = RegisterServiceCtrlHandlerExA("FastCached", &ServiceCtrlHandlerEx, nullptr);
        if (!g_serviceStatus)
            return;
        g_currentStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
        ReportStatus(SERVICE_START_PENDING, 5000);
        ReportStatus(SERVICE_RUNNING);

        if (g_body)
            g_exitCode.store(g_body(), std::memory_order_release);

        ReportStatus(SERVICE_STOPPED);
    }

    class WindowsServiceHost final: public IDaemonHost
    {
      public:
        explicit WindowsServiceHost(std::string name) noexcept: _name { std::move(name) } {}

        int Run(Body body) override
        {
            g_body = std::move(body);
            // SERVICE_TABLE_ENTRYA takes a mutable char*; the SCM does not
            // modify the name but the API signature requires non-const.
            SERVICE_TABLE_ENTRYA table[] = {
                { _name.data(), &ServiceMain },
                { nullptr, nullptr },
            };
            if (!StartServiceCtrlDispatcherA(table))
                return 1;
            return g_exitCode.load(std::memory_order_acquire);
        }

      private:
        std::string _name;
    };

} // namespace

std::unique_ptr<IDaemonHost> MakeWindowsServiceHost(std::string serviceName)
{
    return std::make_unique<WindowsServiceHost>(std::move(serviceName));
}

#endif // _WIN32

} // namespace FastCache
