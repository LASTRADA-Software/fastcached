// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace FastCache
{

/// @file ServiceStatusPlan.hpp
/// What a Windows service host tells the service control manager, and in which order -- as data,
/// so the order is testable on every platform while `WindowsServiceHost.cpp` only walks it.
///
/// ## Why a start that is refused is its own row
///
/// The SCM waits 30 seconds for a service it started to CONNECT, and reads everything after that
/// from the states the service reports. A process that exits before connecting -- which is what a
/// startup refusal did, since every one of them returned from `main` before the host was entered --
/// is reported by the SCM as error 1053, *did not respond to the start or control request in a
/// timely fashion*: an operator running `sc start` saw a hang, and the refusal itself reached only
/// the Application log. A refused start therefore connects, and reports ONE state: stopped, with the
/// process's own exit code as the service-specific code. It never reports RUNNING first, or
/// `sc start` would answer success for a service that is already stopping.

/// Why a service host is reporting to the SCM.
///
/// **PRIVATE: persisted and transmitted nowhere.** No explicit values, by the rule for private enums.
enum class ServiceHostStart : std::uint8_t
{
    Serving, ///< The body runs: the SCM is told it is starting, then running, then how it stopped.
    Refused, ///< The configuration was refused before any body could run: the SCM is told it stopped.
    Last,    ///< Not a start.
};

/// A state the SCM is told, as `SERVICE_STATUS::dwCurrentState` spells it on Windows.
///
/// **PRIVATE: persisted and transmitted nowhere** -- mapped to the Win32 constant in
/// `WindowsServiceHost.cpp`, never cast.
enum class ServiceState : std::uint8_t
{
    StartPending, ///< `SERVICE_START_PENDING`.
    Running,      ///< `SERVICE_RUNNING`.
    StopPending,  ///< `SERVICE_STOP_PENDING`.
    Stopped,      ///< `SERVICE_STOPPED`.
};

/// One report made before the body runs.
struct ServiceStateReport
{
    ServiceState state;       ///< What the SCM is told.
    std::uint32_t waitHintMs; ///< How long a pending state expects to take; zero otherwise.
};

/// What a service that is about to serve reports before its body runs.
inline constexpr std::array ServingReports {
    ServiceStateReport { .state = ServiceState::StartPending, .waitHintMs = 5'000 },
    ServiceStateReport { .state = ServiceState::Running, .waitHintMs = 0 },
};

/// What a refused start reports before it stops: nothing at all.
inline constexpr std::array<ServiceStateReport, 0> RefusedReports {};

/// One kind of start, and what it reports before the stop that ends every run.
struct ServiceHostStartRow
{
    /// The enumerator this row describes. `Last` until a row says otherwise, so a row nobody
    /// wrote fails `RowsInEnumeratorOrder` rather than claiming to be `Serving`.
    ServiceHostStart start { ServiceHostStart::Last };
    std::span<ServiceStateReport const> beforeStop; ///< Reported in order; the stop follows.
};

/// Every kind of start, in enumerator order.
inline constexpr EnumTable<ServiceHostStart, ServiceHostStartRow> ServiceHostStartTable { {
    { .start = ServiceHostStart::Serving, .beforeStop = ServingReports },
    { .start = ServiceHostStart::Refused, .beforeStop = RefusedReports },
} };
static_assert(RowsInEnumeratorOrder(ServiceHostStartTable, &ServiceHostStartRow::start),
              "ServiceHostStartTable must hold one row per ServiceHostStart, in enumerator order");

/// `ERROR_SERVICE_SPECIFIC_ERROR`, spelled without `<windows.h>` so the plan is testable
/// everywhere; `WindowsServiceHost.cpp` asserts it equals the real constant.
///
/// What `sc query` prints as `WIN32_EXIT_CODE : 1066 (0x42a)`, meaning "the reason is in
/// `SERVICE_EXIT_CODE`" -- which carries the process's own exit code: `78` for a start either
/// daemon refuses, on its configuration or a defect in its own build (`ProcessExit::Refused`), `1`
/// for one that failed (`ProcessExit::Failed`).
inline constexpr std::uint32_t ServiceSpecificError = 1066;

/// The exit fields of a `SERVICE_STOPPED` report.
struct ServiceExit
{
    std::uint32_t win32ExitCode;           ///< `dwWin32ExitCode`.
    std::uint32_t serviceSpecificExitCode; ///< `dwServiceSpecificExitCode`.

    [[nodiscard]] friend constexpr bool operator==(ServiceExit const&, ServiceExit const&) = default;
};

/// How a run that ended with @p exitCode is reported stopped.
///
/// Zero stays `NO_ERROR`, so a clean stop reads as one and no failure action restarts a service an
/// operator asked to stop. Anything else is the out-of-band "the specific code is in the other
/// field", never a Win32 error number this process never had -- which is also what lets the
/// restart policy registered at install fire on an ordinary failure.
/// @param exitCode The process's exit code.
/// @return The two fields.
[[nodiscard]] constexpr ServiceExit ServiceExitFor(int exitCode) noexcept
{
    if (exitCode == 0)
        return ServiceExit { .win32ExitCode = 0, .serviceSpecificExitCode = 0 };
    return ServiceExit { .win32ExitCode = ServiceSpecificError,
                         .serviceSpecificExitCode = static_cast<std::uint32_t>(exitCode) };
}

} // namespace FastCache
