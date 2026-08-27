// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/Logger.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace FastCache
{

/// The three severities a Windows event log record can carry.
///
/// Named here rather than used as the Win32 constants directly so the mapping below
/// is a pure function this project can assert on every platform -- the same reason
/// `BuildLaunchdPlist` is pure and tested off macOS. The `.cpp` translates these to
/// `EVENTLOG_*` at the one call site that needs them.
enum class EventLogSeverity : std::uint8_t
{
    Information,
    Warning,
    Error,
};

/// Which event-log severity a log level is recorded as.
///
/// Six levels fold onto three, because three is what the log has. Warn is the
/// boundary deliberately: below it everything is informational, Trace and Debug
/// included -- an operator who turned those on is reading them on purpose -- and
/// `Fatal` is an error rather than a fourth severity that does not exist here.
///
/// A `switch` with no `default:` rather than an `EnumTable`, for the reason
/// `ToStringView` in `Core/Logger` is one: `LogLevel` declares no count, so there is
/// no extent to anchor a table on. Under this project's pedantic settings an
/// unhandled enumerator here is a build failure, which is the guarantee
/// `RowsInEnumeratorOrder` gives elsewhere -- a seventh level stops the build rather
/// than quietly logging as information.
///
/// @param level The level of the record.
/// @return The severity the event log should record it under.
[[nodiscard]] constexpr EventLogSeverity EventLogSeverityFor(LogLevel level) noexcept
{
    switch (level)
    {
        case LogLevel::Trace:
        case LogLevel::Debug:
        case LogLevel::Info:
            return EventLogSeverity::Information;
        case LogLevel::Warn:
            return EventLogSeverity::Warning;
        case LogLevel::Error:
        case LogLevel::Fatal:
            return EventLogSeverity::Error;
    }
    return EventLogSeverity::Information; // Unreachable; a cast from an invalid value.
}

/// An `ILogger` that writes to the Windows **Application event log**.
///
/// A service started by the SCM has no console, and the SCM redirects nothing, so
/// every line written to stderr is discarded. What is discarded is not incidental:
/// it is the only place a toolchain survey and its fingerprints, a scheduler's own
/// words for refusing a registration, a failure to write fleet history, and any
/// refusal to start at all are reported. A registered service that is failing then
/// looks exactly like one that is working -- `/healthz` answers `OK` to both,
/// because it is liveness rather than correctness (#179).
///
/// The event log rather than a file, because it is what the other two supervisors
/// already have and what an operator on this platform already looks at. systemd
/// hands stderr to the journal; launchd is given `StandardErrorPath`; Windows has
/// the event log, and reaching for a file instead would have meant inventing a
/// directory, an access list on it, and a rotation policy for it -- three problems
/// this platform had already solved. `Get-WinEvent -LogName Application
/// -ProviderName <service>` is the whole retrieval story.
///
/// It also keeps the SEVERITY that a byte stream throws away: `ILogger` already
/// carries a level, and the event log is the one sink here that records it as
/// something an operator can filter on rather than as four characters inside a line.
///
/// The source must be registered for the text to render without a
/// "description not found" preamble; `InstallService` does that, and
/// `UninstallService` removes it.
///
/// @param sourceName Event source, which is the service name. Two services on one
///        machine are then two providers, filterable apart.
/// @param minLevel Initial filter threshold.
/// @return The logger, or **nullptr** on any platform without an event log -- so a
///         caller falls back to its console logger by asking for one and checking.
[[nodiscard]] std::unique_ptr<ILogger> MakeWindowsEventLogger(std::string const& sourceName, LogLevel minLevel);

} // namespace FastCache
