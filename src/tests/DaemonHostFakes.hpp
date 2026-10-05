// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <FastCache/Core/EnumTable.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/IServiceControlManager.hpp>
#include <FastCache/Platform/ServiceStatusPlan.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FastCache::Testing
{

/// An `IDaemonHost` that records what it was asked to do and what it answered.
///
/// **Shared, for the rulebook's reason about fakes**: a copy per case is a copy that can be
/// wrong, and a wrong fake makes its cases pass. It stands in for the SCM host in particular --
/// the one host whose `Refuse` is not "return the code" -- so it answers `Refuse` with a code of
/// its OWN choosing, which is what lets a case tell "the caller returned what the host said" from
/// "the caller returned its own constant and never asked".
class RecordingDaemonHost final: public IDaemonHost
{
  public:
    /// @param refusalAnswer What `Refuse` returns, whatever it is asked with.
    explicit RecordingDaemonHost(int refusalAnswer) noexcept:
        _refusalAnswer { refusalAnswer }
    {
    }

    /// Runs @p body, as the foreground host does, and counts it.
    /// @param body The daemon body.
    /// @return The body's exit code.
    [[nodiscard]] int Run(Body body) override
    {
        ++_runs;
        return body ? body() : 0;
    }

    /// Records @p exitCode and answers the code this fake was built with.
    /// @param exitCode The refusal's code.
    /// @return The configured answer.
    [[nodiscard]] int Refuse(int exitCode) override
    {
        _refusals.push_back(exitCode);
        return _refusalAnswer;
    }

    /// @return How many bodies were run.
    [[nodiscard]] std::size_t Runs() const noexcept
    {
        return _runs;
    }

    /// @return Every code `Refuse` was asked with, in order.
    [[nodiscard]] std::vector<int> const& Refusals() const noexcept
    {
        return _refusals;
    }

  private:
    int _refusalAnswer;
    std::size_t _runs = 0;
    std::vector<int> _refusals;
};

/// The Win32 errors a refused `ControlService` answers with (`winerror.h`). The VALUES are
/// Windows', which a case compares against, so each is spelled rather than enumerated.
namespace ScmError
{
    /// `ERROR_INVALID_SERVICE_CONTROL`: the state allows the control, the service does not accept it.
    inline constexpr std::uint32_t InvalidServiceControl = 1052;
    /// `ERROR_SERVICE_CANNOT_ACCEPT_CTRL`: the STATE allows no such control, whatever is accepted.
    inline constexpr std::uint32_t ServiceCannotAcceptCtrl = 1061;
    /// `ERROR_SERVICE_NOT_ACTIVE`: the service is stopped.
    inline constexpr std::uint32_t ServiceNotActive = 1062;
} // namespace ScmError

/// What the SCM does with a control in one state: the three actions of the `ControlService`
/// remarks table (https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-controlservice).
///
/// Private to the fake: never transmitted or persisted.
enum class ScmControlAction : std::uint8_t
{
    SendIfAccepted, ///< (a): send it if the last report accepts it, else `InvalidServiceControl`.
    CannotAccept,   ///< (b): `ServiceCannotAcceptCtrl`, the handler never called.
    NotActive,      ///< (c): `ServiceNotActive`, the handler never called.
    Last,           ///< Not an action.
};

/// The error an action refuses with.
struct ScmControlActionRow
{
    ScmControlAction action { ScmControlAction::Last }; ///< The enumerator this row describes.
    std::uint32_t refusal { 0 };                        ///< What a refused control answers.
};

/// Every action, in enumerator order.
inline constexpr EnumTable<ScmControlAction, ScmControlActionRow> ScmControlActionTable { {
    { .action = ScmControlAction::SendIfAccepted, .refusal = ScmError::InvalidServiceControl },
    { .action = ScmControlAction::CannotAccept, .refusal = ScmError::ServiceCannotAcceptCtrl },
    { .action = ScmControlAction::NotActive, .refusal = ScmError::ServiceNotActive },
} };
static_assert(RowsInEnumeratorOrder(ScmControlActionTable, &ScmControlActionRow::action),
              "ScmControlActionTable must hold one row per ScmControlAction, in enumerator order");

/// The SCM's action per reported state, for a stop and for every other control.
struct ScmStateActionRow
{
    ServiceState state { ServiceState::Last };                 ///< The enumerator this row describes.
    ScmControlAction stop { ScmControlAction::Last };          ///< `SERVICE_CONTROL_STOP`.
    ScmControlAction otherControls { ScmControlAction::Last }; ///< Every other control.
};

/// The `ControlService` remarks table, row for row, for the states a `ServiceState` names.
///
/// **The STATE decides first**: in START_PENDING only a stop can be sent, and a reload is refused
/// `ServiceCannotAcceptCtrl` (1061) however the report's acceptance reads -- never 1052, which is
/// the answer only where the state allows the control. A fake that decided from the acceptance bits
/// alone delivered what Windows never delivers (round 11 review, I1). `SERVICE_CONTROL_SHUTDOWN`
/// and power broadcasts come from the system rather than `ControlService`, so the table does not
/// cover them; they take the OTHER column here, the narrower reading, because a model more
/// permissive than the manager produces wrong agreement.
inline constexpr EnumTable<ServiceState, ScmStateActionRow> ScmStateActionTable { {
    { .state = ServiceState::StartPending,
      .stop = ScmControlAction::SendIfAccepted,
      .otherControls = ScmControlAction::CannotAccept },
    { .state = ServiceState::Running,
      .stop = ScmControlAction::SendIfAccepted,
      .otherControls = ScmControlAction::SendIfAccepted },
    { .state = ServiceState::StopPending,
      .stop = ScmControlAction::CannotAccept,
      .otherControls = ScmControlAction::CannotAccept },
    { .state = ServiceState::Stopped, .stop = ScmControlAction::NotActive, .otherControls = ScmControlAction::NotActive },
} };
static_assert(RowsInEnumeratorOrder(ScmStateActionTable, &ScmStateActionRow::state),
              "ScmStateActionTable must hold one row per ServiceState, in enumerator order");

/// Whether a scripted service manager has a manager to connect to at all.
///
/// Private to the fake: never transmitted or persisted.
enum class ServiceManagerPresence : std::uint8_t
{
    Present, ///< The process was started as a service.
    Absent,  ///< Started by hand: `Dispatch` has nothing to connect to.
};

/// An `IServiceControlManager` that runs the service main inline and holds it to the SCM's rules.
///
/// **A fake more permissive than the manager it stands for makes its cases pass over a defect**,
/// so every rule the SCM enforces or documents is a recorded VIOLATION here: one dispatch per
/// process, no status before the control handler is registered, the handler registered once and
/// only from inside the service main, a legal order of states, a start's and a stop's checkpoint
/// that only ever advance, and nothing at all after `STOPPED`. A case asserts `Violations()` is empty beside
/// what it asserts about the reports. Thread-safe where the SCM is reached from more than one
/// thread -- a stop's progress is reported from a reporter of its own while the body runs -- so a
/// case may read the reports while they arrive.
/// Presence is REQUIRED and undefaulted: whether a manager exists is the first thing a case about
/// a service host has to decide.
///
/// **Its blind spot is its threading, by construction**: it runs the service main INLINE, on the
/// thread that called `Dispatch`. The real `StartServiceCtrlDispatcher` runs it on a thread of its
/// own and RETURNS once `STOPPED` is recorded -- not once that thread has finished -- so `main`
/// may destroy the host while the service thread is still inside the call that reported the stop.
/// Nothing here can show that happening. `AfterStopped` is what a case uses to stand in the
/// window: what it runs, runs where the real dispatcher would already have returned.
class ScriptedServiceControlManager final: public IServiceControlManager
{
  public:
    /// @param presence Whether a manager answers `Dispatch`.
    explicit ScriptedServiceControlManager(ServiceManagerPresence presence) noexcept:
        _presence { presence }
    {
    }

    /// Runs @p main inline, once per process, as the dispatcher would on a thread of its own.
    /// @param name Ignored.
    /// @param main The service main.
    /// @return `NoServiceManager` when absent, `AlreadyRunning` on a second dispatch.
    [[nodiscard]] DispatchOutcome Dispatch(std::string const& /*name*/, ServiceMain main) override
    {
        ++_dispatches;
        if (_presence == ServiceManagerPresence::Absent)
            return DispatchOutcome::NoServiceManager;
        if (_dispatched)
            return DispatchOutcome::AlreadyRunning;
        _dispatched = true;
        _inServiceMain = true;
        if (main)
            main();
        _inServiceMain = false;
        std::scoped_lock const lock { _mutex };
        if (_handlerRegistered && !_stopped)
            _violations.emplace_back("the service main returned without reporting STOPPED");
        return DispatchOutcome::Dispatched;
    }

    /// Records the handler.
    /// @param name Ignored.
    /// @param handler What controls are handed to.
    /// @return True.
    [[nodiscard]] bool RegisterHandler(std::string const& /*name*/, ControlHandler handler) override
    {
        std::scoped_lock const lock { _mutex };
        if (!_inServiceMain)
            _violations.emplace_back("the control handler was registered outside the service main");
        if (_handlerRegistered)
            _violations.emplace_back("the control handler was registered twice");
        _handlerRegistered = true;
        _handler = std::move(handler);
        return true;
    }

    /// Records @p report, and every rule it breaks; runs `AfterStopped`'s action on the stop.
    /// @param report The status.
    void SetStatus(ServiceStatusReport const& report) override
    {
        std::unique_lock lock { _mutex };
        if (!_handlerRegistered)
            _violations.emplace_back("a status was set before the control handler was registered");
        if (_stopped)
            _violations.emplace_back("a status was set after STOPPED");
        if (!LegalAfter(report.state))
            _violations.emplace_back("a status was set out of order");
        for (auto const& rule: PendingRules)
            if (report.state == rule.state && !_reports.empty() && _reports.back().state == rule.state
                && report.checkPoint <= _reports.back().checkPoint)
                _violations.emplace_back(rule.violation);
        _stopped = _stopped || report.state == ServiceState::Stopped;
        _reports.push_back(report);
        // Outside this fake's own lock: the action stands in for the world after the dispatcher
        // returned, which a lock in the fake is no part of.
        auto const afterStopped = report.state == ServiceState::Stopped ? _afterStopped : std::function<void()> {};
        lock.unlock();
        if (afterStopped)
            afterStopped();
    }

    /// What happens the moment `STOPPED` is recorded, still inside the call that reported it --
    /// the window in which the real dispatcher has already returned and `main` may be tearing the
    /// host down.
    /// @param action Run once per stop, on the reporting thread.
    void AfterStopped(std::function<void()> action)
    {
        _afterStopped = std::move(action);
    }

    /// A copy of the registered handler, as the SCM keeps a registration: for the life of the
    /// PROCESS, not of the object that made it. Calling it after the host is gone is what a
    /// control that was already on its way when `main` freed the host does.
    /// @return The handler; empty when none is registered.
    [[nodiscard]] ControlHandler RegisteredHandler() const
    {
        return _handler;
    }

    /// Deliver @p request to the registered handler, as the SCM does on a thread of its own: a
    /// control the manager has ALREADY accepted, so it reaches the handler whatever is reported now
    /// -- which is how a stop arrives after STOPPED. `Request` is the call that asks first.
    /// @param request The control.
    /// @param eventType The manager's event type: a `PBT_*` value for a power event.
    /// @return What the handler answered; false when none is registered.
    [[nodiscard]] bool Deliver(ServiceControlRequest request, std::uint32_t eventType = 0) const
    {
        return _handler && _handler(request, eventType);
    }

    /// Send @p request the way `sc stop`, an MSI ServiceControl or a power broadcast does, decided
    /// as the SCM decides it (`ScmStateActionTable`): by the last reported STATE first, then by
    /// what that report accepted. A refusal is the manager's own, with the handler never called.
    /// Before any report the manager holds the service START_PENDING and accepting nothing.
    /// @param request The control.
    /// @param eventType The manager's event type: a `PBT_*` value for a power event.
    /// @return What the handler answered, or the `ScmError` the manager refused the control with.
    [[nodiscard]] std::expected<bool, std::uint32_t> Request(ServiceControlRequest request,
                                                             std::uint32_t eventType = 0) const
    {
        auto const last = LastReport();
        auto const& row = ScmStateActionTable[static_cast<std::size_t>(last.state)];
        auto const action = request == ServiceControlRequest::Stop ? row.stop : row.otherControls;
        if (action != ScmControlAction::SendIfAccepted || !Accepts(last, request))
            return std::unexpected { ScmControlActionTable[static_cast<std::size_t>(action)].refusal };
        return Deliver(request, eventType);
    }

    /// @return Every status reported so far, in order.
    [[nodiscard]] std::vector<ServiceStatusReport> Reports() const
    {
        std::scoped_lock const lock { _mutex };
        return _reports;
    }

    /// @return The states reported, in order.
    [[nodiscard]] std::vector<ServiceState> States() const
    {
        std::scoped_lock const lock { _mutex };
        std::vector<ServiceState> states;
        states.reserve(_reports.size());
        for (auto const& report: _reports)
            states.push_back(report.state);
        return states;
    }

    /// @return Every rule of the SCM's that was broken, in order.
    [[nodiscard]] std::vector<std::string> Violations() const
    {
        std::scoped_lock const lock { _mutex };
        return _violations;
    }

    /// @return How many times `Dispatch` was called.
    [[nodiscard]] std::size_t Dispatches() const noexcept
    {
        return _dispatches;
    }

  private:
    /// One state the SCM accepts after another; the first report follows `std::nullopt`.
    struct Transition
    {
        std::optional<ServiceState> from; ///< The last state reported, or none yet.
        ServiceState to;                  ///< The state that may follow it.
    };

    /// A pending state, whose checkpoint must advance from one report of it to the next, and the
    /// violation that names one that did not.
    struct PendingRule
    {
        ServiceState state;         ///< The pending state.
        std::string_view violation; ///< What a checkpoint that stood still is recorded as.
    };

    static constexpr std::array PendingRules {
        PendingRule { .state = ServiceState::StartPending, .violation = "a start's checkpoint did not advance" },
        PendingRule { .state = ServiceState::StopPending, .violation = "a stop's checkpoint did not advance" },
    };

    static constexpr std::array LegalTransitions {
        Transition { .from = std::nullopt, .to = ServiceState::StartPending },
        Transition { .from = std::nullopt, .to = ServiceState::Stopped },
        Transition { .from = ServiceState::StartPending, .to = ServiceState::StartPending },
        Transition { .from = ServiceState::StartPending, .to = ServiceState::Running },
        Transition { .from = ServiceState::StartPending, .to = ServiceState::StopPending },
        Transition { .from = ServiceState::StartPending, .to = ServiceState::Stopped },
        Transition { .from = ServiceState::Running, .to = ServiceState::StopPending },
        Transition { .from = ServiceState::Running, .to = ServiceState::Stopped },
        Transition { .from = ServiceState::StopPending, .to = ServiceState::StopPending },
        Transition { .from = ServiceState::StopPending, .to = ServiceState::Stopped },
    };

    /// @return The last report, or what the manager holds before one: START_PENDING, accepting
    ///         nothing.
    [[nodiscard]] ServiceStatusReport LastReport() const
    {
        std::scoped_lock const lock { _mutex };
        return _reports.empty() ? ServiceStatusReport { .state = ServiceState::StartPending } : _reports.back();
    }

    /// @return Whether @p report accepted @p request -- asked only where the state lets the control
    ///         be sent. An interrogation is accepted by every service by default (`ControlService`
    ///         remarks), so no report needs to name it.
    [[nodiscard]] static bool Accepts(ServiceStatusReport const& report, ServiceControlRequest request)
    {
        switch (request)
        {
            case ServiceControlRequest::Stop:
            case ServiceControlRequest::Shutdown:
                return report.acceptsStop;
            case ServiceControlRequest::ParamChange:
                return report.acceptsReload;
            case ServiceControlRequest::PowerEvent:
                return report.acceptsPowerEvents;
            case ServiceControlRequest::Interrogate:
                return true;
            case ServiceControlRequest::Unsupported:
                break;
        }
        return false;
    }

    /// @return Whether @p next may follow the last state reported.
    [[nodiscard]] bool LegalAfter(ServiceState next) const
    {
        auto const from = _reports.empty() ? std::optional<ServiceState> {} : std::optional { _reports.back().state };
        return std::ranges::any_of(LegalTransitions, [&](Transition const& transition) {
            return transition.from == from && transition.to == next;
        });
    }

    /// Guards the reports and the violations, which a stop's reporter writes from its own thread.
    mutable std::mutex _mutex;
    ControlHandler _handler;
    std::function<void()> _afterStopped;
    std::vector<ServiceStatusReport> _reports;
    std::vector<std::string> _violations;
    std::size_t _dispatches = 0;
    ServiceManagerPresence _presence;
    bool _dispatched = false;
    bool _inServiceMain = false;
    bool _handlerRegistered = false;
    bool _stopped = false;
};

} // namespace FastCache::Testing
