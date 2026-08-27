// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Platform/WindowsEventLogger.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
#endif

namespace FastCache
{

#if !defined(_WIN32)

std::unique_ptr<ILogger> MakeWindowsEventLogger(std::string const& /*sourceName*/, LogLevel /*minLevel*/)
{
    return nullptr; // No event log here; the caller keeps its console logger.
}

#else

namespace
{

    /// The message id every record is reported under.
    ///
    /// One, because the source is registered against a message resource whose first
    /// entry is the passthrough `%1` -- so the single insertion string below renders
    /// verbatim. This project does not ship a message DLL of its own, and shipping
    /// one to say `%1` would be a build artefact whose entire content is a formatting
    /// instruction.
    constexpr DWORD PassthroughMessageId = 1;

    /// The Win32 constant for a severity.
    ///
    /// The one place `EventLogSeverity` meets `<windows.h>`, so the decision above
    /// stays assertable on a platform that has no such header.
    /// @param severity What `EventLogSeverityFor` decided.
    /// @return `EVENTLOG_ERROR_TYPE`, `EVENTLOG_WARNING_TYPE` or
    ///         `EVENTLOG_INFORMATION_TYPE`.
    [[nodiscard]] WORD EventTypeFor(EventLogSeverity severity) noexcept
    {
        switch (severity)
        {
            case EventLogSeverity::Information:
                return EVENTLOG_INFORMATION_TYPE;
            case EventLogSeverity::Warning:
                return EVENTLOG_WARNING_TYPE;
            case EventLogSeverity::Error:
                return EVENTLOG_ERROR_TYPE;
        }
        return EVENTLOG_INFORMATION_TYPE; // Unreachable; a cast from an invalid value.
    }

    /// Writes each record to the Application event log as one event.
    class WindowsEventLogger final: public ILogger
    {
      public:
        /// @param sourceName Event source; the service name.
        /// @param minLevel Initial filter threshold.
        WindowsEventLogger(std::string const& sourceName, LogLevel minLevel) noexcept:
            _minLevel { minLevel },
            _handle { RegisterEventSourceA(nullptr, sourceName.c_str()) }
        {
        }

        WindowsEventLogger(WindowsEventLogger const&) = delete;
        WindowsEventLogger(WindowsEventLogger&&) = delete;
        WindowsEventLogger& operator=(WindowsEventLogger const&) = delete;
        WindowsEventLogger& operator=(WindowsEventLogger&&) = delete;

        ~WindowsEventLogger() override
        {
            if (_handle != nullptr)
                DeregisterEventSource(_handle);
        }

        void Log(LogLevel level, std::string_view message) override
        {
            // Filtered here as well as by every caller, because `ILogger` promises it
            // and a sink that logged everything would be a level flag that stopped
            // working the moment somebody called `Log` directly.
            if (level < _minLevel.load(std::memory_order_acquire))
                return;

            // A copy, because the API takes a null-terminated pointer and a
            // `string_view` is not one -- the caller's buffer may be a slice of a
            // larger line.
            std::string const text { message };

            // Not `const*`-qualified twice: `ReportEventA` takes `LPCSTR*`, a mutable
            // array of pointers to const, and a pointer that is itself const does not
            // convert to one.
            auto const* insert = text.c_str();

            // SYNCHRONOUS, and that is a real cost rather than an oversight:
            // `ReportEventA` is an RPC to the EventLog service, so this is heavier
            // than `ConsoleLogger`'s buffered write and it is paid on whichever
            // thread logged -- a reactor thread included. At the level a service
            // actually runs at that is a handful of records per heartbeat round and
            // costs nothing measurable; at `--log-level=debug` or `trace` it is one
            // round trip per record, on a path that should not be doing round trips.
            // Buffering it behind a queue is issue #210 rather than a line here,
            // because a queue that outlives its logger is its own hazard and this
            // sink must keep working while the process is exiting.
            //
            // Best effort otherwise: a service must not fail because the event log
            // did, and there is nowhere left to report a failure TO -- reporting it
            // to stderr would be writing to the handle this class exists because
            // nobody holds.
            static_cast<void>(ReportEventA(_handle,
                                           EventTypeFor(EventLogSeverityFor(level)),
                                           /*wCategory*/ WORD { 0 },
                                           PassthroughMessageId,
                                           /*lpUserSid*/ nullptr,
                                           /*wNumStrings*/ 1,
                                           /*dwDataSize*/ 0,
                                           &insert,
                                           /*lpRawData*/ nullptr));
        }

        [[nodiscard]] LogLevel MinLevel() const noexcept override
        {
            return _minLevel.load(std::memory_order_acquire);
        }

        void SetMinLevel(LogLevel level) noexcept override
        {
            _minLevel.store(level, std::memory_order_release);
        }

        /// @return Whether the source was opened; false means this sink can log
        ///         nothing at all, and the caller must fall back rather than hold it.
        [[nodiscard]] bool Usable() const noexcept
        {
            return _handle != nullptr;
        }

      private:
        std::atomic<LogLevel> _minLevel;

        /// Null when the source could not be opened. The factory refuses to hand
        /// such a logger out at all -- see there for why.
        HANDLE _handle;
    };

} // namespace

std::unique_ptr<ILogger> MakeWindowsEventLogger(std::string const& sourceName, LogLevel minLevel)
{
    auto logger = std::make_unique<WindowsEventLogger>(sourceName, minLevel);

    // A source that could not be opened is answered as ABSENT, not as a sink that
    // silently discards. Returning the logger anyway would satisfy the caller's
    // null check, take the console fallback out of reach, and leave the service
    // logging precisely nowhere -- which is the defect this whole file exists to
    // remove, reintroduced one layer up.
    if (!logger->Usable())
        return nullptr;
    return logger;
}

#endif // _WIN32

} // namespace FastCache
