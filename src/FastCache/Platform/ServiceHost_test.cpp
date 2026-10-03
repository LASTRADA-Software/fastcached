// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/ServiceHost.hpp>
#include <FastCache/Platform/StopPending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <tests/BoundedWait.hpp>
#include <tests/DaemonHostFakes.hpp>
#include <tests/Unwrap.hpp>

using namespace FastCache;
using Testing::ScriptedServiceControlManager;
using Testing::ServiceManagerPresence;

namespace
{

/// What a stop reports in these cases: a hint and a checkpoint cadence a case can see advance.
constexpr StopPendingPlan TestStopPlan { .waitHint = std::chrono::milliseconds { 7'000 },
                                         .checkpointEvery = std::chrono::milliseconds { 1 } };

/// A host-event sink that records what it was handed, from whichever thread delivered it.
class RecordingHostEventSink final: public IHostEventSink
{
  public:
    void OnHostEvent(HostEvent event) override
    {
        std::scoped_lock const lock { _mutex };
        _events.push_back(event);
    }

    /// @return Every event delivered, in order.
    [[nodiscard]] std::vector<HostEvent> Events() const
    {
        std::scoped_lock const lock { _mutex };
        return _events;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<HostEvent> _events;
};

/// A service host over a scripted manager, and the manager, which the host owns.
struct ScriptedService
{
    /// @param presence Whether a manager answers.
    /// @param hostEvents Where power events go, or null for a service that accepts none.
    explicit ScriptedService(ServiceManagerPresence presence, IHostEventSink* hostEvents = nullptr)
    {
        auto owned = std::make_unique<ScriptedServiceControlManager>(presence);
        manager = owned.get();
        host = std::make_unique<ServiceHost>("FastCachedTest",
                                             std::move(owned),
                                             controls,
                                             ServiceHostOptions { .stop = TestStopPlan, .hostEvents = hostEvents },
                                             DefaultDrainWait());
    }

    DaemonControls controls;                          ///< What a control raises.
    ScriptedServiceControlManager* manager = nullptr; ///< Owned by `host`.
    std::unique_ptr<ServiceHost> host;                ///< The subject.
};

/// @p states with every run of one state folded to one: a stop reports `StopPending` as often as
/// its checkpoint advances, which depends on how long the body takes to return.
[[nodiscard]] std::vector<ServiceState> Collapsed(std::vector<ServiceState> states)
{
    auto const [first, last] = std::ranges::unique(states);
    states.erase(first, last);
    return states;
}

/// @return The checkpoints of every `StopPending` report, in order.
[[nodiscard]] std::vector<std::uint32_t> StopCheckpoints(ScriptedServiceControlManager const& manager)
{
    std::vector<std::uint32_t> points;
    for (auto const& report: manager.Reports())
        if (report.state == ServiceState::StopPending)
            points.push_back(report.checkPoint);
    return points;
}

} // namespace

TEST_CASE("A refused service start reports one stop carrying its exit code", "[platform][service][refusal]")
{
    // What `sc query` shows for a start refused by its configuration: STOPPED, WIN32_EXIT_CODE
    // 1066 and SERVICE_EXIT_CODE the process's own code. Never a START_PENDING or RUNNING first:
    // `sc start` would then answer success for a service that is already stopping.
    ScriptedService service { ServiceManagerPresence::Present };

    CHECK(service.host->Refuse(2) == 2);

    CHECK(service.manager->Violations().empty());
    CHECK(service.manager->Reports()
          == std::vector {
              ServiceStatusReport { .state = ServiceState::Stopped,
                                    .waitHintMs = 0,
                                    .exit =
                                        ServiceExit { .win32ExitCode = ServiceSpecificError, .serviceSpecificExitCode = 2 },
                                    .acceptsControls = false },
          });
    CHECK_FALSE(service.controls.StopRequested());
}

TEST_CASE("A serving start reports starting and running before its body and its exit code after", "[platform][service]")
{
    // The control on the case above: the same host walking the serving row.
    ScriptedService service { ServiceManagerPresence::Present };
    auto runs = 0;

    auto const body = [&runs] {
        ++runs;
        return 3;
    };
    CHECK(service.host->Run(body) == 3);

    CHECK(runs == 1);
    CHECK(service.manager->Violations().empty());
    CHECK(service.manager->States()
          == std::vector { ServiceState::StartPending, ServiceState::Running, ServiceState::Stopped });
    REQUIRE(service.manager->Reports().size() == 3);
    CHECK(service.manager->Reports()[1].acceptsControls);
    CHECK(service.manager->Reports().back().exit
          == ServiceExit { .win32ExitCode = ServiceSpecificError, .serviceSpecificExitCode = 3 });

    SECTION("and a clean stop reads as one, so no restart policy fires")
    {
        ScriptedService clean { ServiceManagerPresence::Present };
        CHECK(clean.host->Run([] { return 0; }) == 0);
        REQUIRE_FALSE(clean.manager->Reports().empty());
        CHECK(clean.manager->Reports().back().exit == ServiceExit { .win32ExitCode = 0, .serviceSpecificExitCode = 0 });
    }
}

TEST_CASE("A service stops and reloads on the controls its manager delivers", "[platform][service]")
{
    ScriptedService service { ServiceManagerPresence::Present };
    std::vector<bool> handled;

    CHECK(service.host->Run([&service, &handled] {
        handled.push_back(service.manager->Deliver(ServiceControlRequest::ParamChange));
        handled.push_back(service.manager->Deliver(ServiceControlRequest::Unsupported));
        handled.push_back(service.manager->Deliver(ServiceControlRequest::Stop));
        return 0;
    }) == 0);

    CHECK(handled == std::vector { true, false, true });
    CHECK(service.controls.TakeReloadRequest());
    CHECK(service.controls.StopRequested());
    CHECK(service.manager->Violations().empty());
    CHECK(Collapsed(service.manager->States())
          == std::vector {
              ServiceState::StartPending, ServiceState::Running, ServiceState::StopPending, ServiceState::Stopped });
}

TEST_CASE("Nothing is reported to a service manager after the stop", "[platform][service][refusal]")
{
    // The SCM calls the control handler on a thread of its own, so a stop can arrive after the
    // service main has reported STOPPED. Telling the manager STOP_PENDING then would leave it the
    // last word -- and a stop with no exit code is a failure no recovery action ever sees.
    ScriptedService service { ServiceManagerPresence::Present };
    CHECK(service.host->Refuse(2) == 2);

    CHECK(service.manager->Deliver(ServiceControlRequest::Stop));

    CHECK(service.manager->Violations().empty());
    CHECK(service.manager->States() == std::vector { ServiceState::Stopped });
}

TEST_CASE("Reporting the stop is the last thing a service host does", "[platform][service][refusal]")
{
    // Under the SCM, `StartServiceCtrlDispatcher` returns on the main thread once STOPPED is
    // RECORDED, and `main` then destroys the host -- while the service thread is still inside the
    // call that reported it. So the host must hold nothing of its own across that call: a lock
    // released after it is released in freed memory. Seen here through a stop that arrives from
    // the manager's own thread in exactly that window: it passes through the host's lock and
    // answers, which it cannot while the reporting thread still holds the lock.
    ScriptedService service { ServiceManagerPresence::Present };
    std::atomic<bool> answered { false };
    std::optional<bool> passedTheLock;
    std::jthread manager; // after `service`, so it is joined before the host goes
    service.manager->AfterStopped([&] {
        manager = std::jthread { [&] {
            std::ignore = service.manager->Deliver(ServiceControlRequest::Stop);
            answered.store(true);
        } };
        passedTheLock = Testing::WaitUntil(
            "a stop delivered while STOPPED is being reported to pass the host's lock",
            [&] { return answered.load(); },
            [&] { return std::string { answered.load() ? "answered" : "still waiting on the host" }; });
    });

    CHECK(service.host->Refuse(78) == 78);

    REQUIRE(passedTheLock.has_value());
    CHECK(Testing::Unwrap(passedTheLock));
    CHECK(service.manager->Violations().empty());
    CHECK(service.manager->States() == std::vector { ServiceState::Stopped });
}

TEST_CASE("A control that arrives after the host is gone reaches nothing of it", "[platform][service][refusal]")
{
    // The SCM calls the control handler on a thread of its own and keeps the registration for the
    // life of the process, while `main` frees the host as soon as the dispatcher returns. So a
    // control already on its way -- or arriving then -- must reach nothing the host owned: the
    // handler holds a lifetime guard that the host closes before any of its members go.
    //
    // The registration is taken where the real dispatcher has already returned (`AfterStopped`),
    // the host is then freed the way `main` frees it, and the control is delivered from another
    // thread. Before the guard, the registration held the host's `this`, and this call ran the
    // host's handler in freed memory -- undefined behaviour, so its red takes either form: usually
    // an ACCESS VIOLATION that ends the binary (measured: 2 runs of 3), otherwise the bounded wait
    // below running out on a lock read from freed memory. A crash here is this case's red, not a
    // different defect.
    auto service = std::make_unique<ScriptedService>(ServiceManagerPresence::Present);
    IServiceControlManager::ControlHandler registration;
    service->manager->AfterStopped([&] { registration = service->manager->RegisteredHandler(); });

    CHECK(service->host->Refuse(78) == 78);
    REQUIRE(registration);
    service.reset();

    std::atomic<int> answer { -1 };
    std::jthread scm { [&] { answer.store(registration(ServiceControlRequest::Stop, 0) ? 1 : 0); } };
    auto const answered = Testing::WaitUntil(
        "a stop delivered after the host was freed to be answered",
        [&] { return answer.load() != -1; },
        [&] { return std::string { answer.load() == -1 ? "still inside the freed host" : "answered" }; });
    if (!answered)
        scm.detach(); // parked in freed memory: joining it would hang the run rather than fail it
    REQUIRE(answered);
    // Answered, and refused: nothing is left to stop.
    CHECK(answer.load() == 0);
}

TEST_CASE("A refused start runs no body after it", "[platform][service][refusal]")
{
    // A process dispatches once: the refusal took the dispatcher, so a Run after it has no service
    // main to run in, and must not run the body outside one.
    ScriptedService service { ServiceManagerPresence::Present };
    auto runs = 0;

    CHECK(service.host->Refuse(2) == 2);
    auto const body = [&runs] {
        ++runs;
        return 0;
    };
    CHECK(service.host->Run(body) == ServiceNotDispatchedExit);

    CHECK(runs == 0);
    CHECK(service.manager->Dispatches() == 2);
    CHECK(service.manager->Violations().empty());
    CHECK(service.manager->States() == std::vector { ServiceState::Stopped });
}

TEST_CASE("Without a service manager a refusal is its exit code and nothing runs", "[platform][service][refusal]")
{
    // Started by hand with `--daemon`: there is nobody to tell, and the exit code is the report.
    ScriptedService service { ServiceManagerPresence::Absent };
    auto runs = 0;

    CHECK(service.host->Refuse(2) == 2);
    auto const body = [&runs] {
        ++runs;
        return 0;
    };
    CHECK(service.host->Run(body) == ServiceNotDispatchedExit);

    CHECK(runs == 0);
    CHECK(service.manager->Reports().empty());
    CHECK(service.manager->Violations().empty());
}

TEST_CASE("The scripted service manager holds a caller to the SCM's rules", "[platform][service][fake]")
{
    // The fake's own case: a manager that accepted what the SCM refuses would pass every case
    // above over a defect. Each rule is broken once and must be named.
    ScriptedServiceControlManager manager { ServiceManagerPresence::Present };
    CHECK(manager.Dispatch("x", [&manager] {
        manager.SetStatus(
            ServiceStatusReport { .state = ServiceState::Running, .waitHintMs = 0, .exit = {}, .acceptsControls = false });
        std::ignore = manager.RegisterHandler("x", [](ServiceControlRequest, std::uint32_t) { return true; });
        manager.SetStatus(ServiceStatusReport { .state = ServiceState::StopPending, .waitHintMs = 0, .checkPoint = 1 });
        manager.SetStatus(ServiceStatusReport { .state = ServiceState::StopPending, .waitHintMs = 0, .checkPoint = 1 });
        manager.SetStatus(
            ServiceStatusReport { .state = ServiceState::Stopped, .waitHintMs = 0, .exit = {}, .acceptsControls = false });
        manager.SetStatus(ServiceStatusReport {
            .state = ServiceState::StopPending, .waitHintMs = 0, .exit = {}, .acceptsControls = false });
    }) == DispatchOutcome::Dispatched);
    CHECK(manager.Dispatch("x", [] {}) == DispatchOutcome::AlreadyRunning);

    CHECK(manager.Violations()
          == std::vector<std::string> {
              "a status was set before the control handler was registered",
              "a status was set out of order",
              "a stop's checkpoint did not advance",
              "a status was set after STOPPED",
              "a status was set out of order",
          });
}

TEST_CASE("A stop reports its progress until the body returns, and a second stop does not restart it", "[platform][service]")
{
    // The SCM reads the wait hint as "the next checkpoint arrives within this", and a drain longer
    // than one hint with no checkpoint reads as a hung service -- to `sc stop`, `Stop-Service` and
    // the MSI's ServiceControl wait alike. So the first stop reports the plan's hint and a reporter
    // advances the checkpoint until the body returns; a second stop control must not report the
    // initial `StopPending` again, which would reset the checkpoint mid-drain.
    ScriptedService service { ServiceManagerPresence::Present };
    auto const reached = [&service](std::uint32_t point) {
        return Testing::WaitUntil(
            std::format("the stop's checkpoint to reach {}", point),
            [&service, point] { return std::ranges::contains(StopCheckpoints(*service.manager), point); },
            [&service] { return std::format("{} StopPending report(s)", StopCheckpoints(*service.manager).size()); });
    };
    auto reachedTwo = false;
    auto reachedFour = false;

    CHECK(service.host->Run([&] {
        std::ignore = service.manager->Deliver(ServiceControlRequest::Stop);
        reachedTwo = reached(2);
        std::ignore = service.manager->Deliver(ServiceControlRequest::Shutdown);
        reachedFour = reached(4);
        return 0;
    }) == 0);

    CHECK(reachedTwo);
    CHECK(reachedFour);
    CHECK(service.manager->Violations().empty());
    auto const points = StopCheckpoints(*service.manager);
    REQUIRE(points.size() >= 5);
    // Zero once, first, and then only ever advancing: the second stop reported nothing of its own.
    CHECK(points.front() == 0);
    CHECK(std::ranges::count(points, std::uint32_t { 0 }) == 1);
    CHECK(std::ranges::is_sorted(points));
    CHECK(std::ranges::adjacent_find(points) == points.end());
    for (auto const& report: service.manager->Reports())
        if (report.state == ServiceState::StopPending)
            CHECK(report.waitHintMs == TestStopPlan.waitHint.count());
    CHECK(service.manager->States().back() == ServiceState::Stopped);
}

TEST_CASE("A service handed a host-event sink accepts power broadcasts while it runs, and one without does not",
          "[platform][service]")
{
    // A power event is a hint, delivered synchronously on the manager's thread: the suspend a
    // presence loop withdraws before is bounded by Windows' two seconds for the notification.
    RecordingHostEventSink sink;
    ScriptedService service { ServiceManagerPresence::Present, &sink };
    ScriptedService deaf { ServiceManagerPresence::Present };
    constexpr std::uint32_t Suspend = 0x4;          // PBT_APMSUSPEND
    constexpr std::uint32_t ResumeAutomatic = 0x12; // PBT_APMRESUMEAUTOMATIC
    constexpr std::uint32_t Unnamed = 0x8013;       // PBT_POWERSETTINGCHANGE, which no row names
    std::vector<bool> handled;
    std::vector<bool> deafHandled;

    CHECK(service.host->Run([&] {
        handled.push_back(service.manager->Deliver(ServiceControlRequest::PowerEvent, Suspend));
        handled.push_back(service.manager->Deliver(ServiceControlRequest::PowerEvent, ResumeAutomatic));
        handled.push_back(service.manager->Deliver(ServiceControlRequest::PowerEvent, Unnamed));
        return 0;
    }) == 0);
    CHECK(deaf.host->Run([&] {
        deafHandled.push_back(deaf.manager->Deliver(ServiceControlRequest::PowerEvent, Suspend));
        return 0;
    }) == 0);

    CHECK(handled == std::vector { true, true, true });
    CHECK(sink.Events() == std::vector { HostEvent::Suspending, HostEvent::Resumed });
    CHECK(deafHandled == std::vector { false });

    auto const acceptsPowerWhileRunning = [](ScriptedServiceControlManager const& manager) {
        auto const reports = manager.Reports();
        auto const found = std::ranges::find(reports, ServiceState::Running, &ServiceStatusReport::state);
        return found != reports.end() && found->acceptsPowerEvents;
    };
    CHECK(acceptsPowerWhileRunning(*service.manager));
    CHECK_FALSE(acceptsPowerWhileRunning(*deaf.manager));
    CHECK(service.manager->Violations().empty());
    CHECK(deaf.manager->Violations().empty());
}
