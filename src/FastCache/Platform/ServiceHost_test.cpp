// SPDX-License-Identifier: Apache-2.0
#include <FastCache/Core/BoundedDrain.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/HostEvents.hpp>
#include <FastCache/Platform/ServiceHost.hpp>
#include <FastCache/Platform/StopPending.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
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
#include <tests/SteppedDrainWait.hpp>
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

/// A start that waits for its body to serve: a checkpoint a case can see advance, and a ceiling far
/// enough off that only the ceiling case meets it.
constexpr StartPendingPlan TestStartPlan { .waitHint = std::chrono::milliseconds { 9'000 },
                                           .checkpointEvery = std::chrono::milliseconds { 1 },
                                           .ceiling = std::chrono::hours { 1 } };

/// A service host told to report RUNNING when its body serves, over a scripted manager.
struct ServingWhenReadyService
{
    /// @param plan How the start reports while it waits.
    /// @param wait Where the start's reporter spends its gaps; must outlive this.
    ServingWhenReadyService(StartPendingPlan plan, IDrainWait& wait)
    {
        auto owned = std::make_unique<ScriptedServiceControlManager>(ServiceManagerPresence::Present);
        manager = owned.get();
        host = std::make_unique<ServiceHost>(
            "FastCachedTest",
            std::move(owned),
            controls,
            ServiceHostOptions { .stop = TestStopPlan, .readiness = ServiceReadiness::BodySignals, .start = plan },
            wait);
    }

    DaemonControls controls;                          ///< What the body marks serving.
    ScriptedServiceControlManager* manager = nullptr; ///< Owned by `host`.
    std::unique_ptr<ServiceHost> host;                ///< The subject.
};

/// @return The checkpoints of every `StartPending` report, in order.
[[nodiscard]] std::vector<std::uint32_t> StartCheckpoints(ScriptedServiceControlManager const& manager)
{
    std::vector<std::uint32_t> points;
    for (auto const& report: manager.Reports())
        if (report.state == ServiceState::StartPending)
            points.push_back(report.checkPoint);
    return points;
}

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
                                    .acceptsStop = false },
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
    CHECK(service.manager->Reports()[1].acceptsStop);
    CHECK(service.manager->Reports()[1].acceptsReload);
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

TEST_CASE("A start told to wait reports RUNNING only once its body serves and advances its checkpoint until then",
          "[platform][service]")
{
    // B4-6: `net start` and an installer's start action return when the service leaves START_PENDING.
    // Reported as the body began, RUNNING answered for a node that then could not bind its port. So
    // the body's own "I serve" is what RUNNING waits for, and the wait shows progress meanwhile.
    ServingWhenReadyService service { TestStartPlan, DefaultDrainWait() };
    auto progressed = false;
    auto runningBeforeServing = true;
    auto runningAfterServing = false;

    CHECK(service.host->Run([&] {
        progressed = Testing::WaitUntil(
            "the start's checkpoint to reach 3",
            [&service] { return std::ranges::contains(StartCheckpoints(*service.manager), 3U); },
            [&service] { return std::format("{} StartPending report(s)", StartCheckpoints(*service.manager).size()); });
        runningBeforeServing = std::ranges::contains(service.manager->States(), ServiceState::Running);
        service.controls.MarkServing();
        runningAfterServing = Testing::WaitUntil(
            "RUNNING after the body served",
            [&service] { return std::ranges::contains(service.manager->States(), ServiceState::Running); },
            [&service] { return std::format("{} report(s)", service.manager->Reports().size()); });
        return 0;
    }) == 0);

    CHECK(progressed);
    CHECK_FALSE(runningBeforeServing);
    CHECK(runningAfterServing);
    CHECK(service.manager->Violations().empty());
    CHECK(Collapsed(service.manager->States())
          == std::vector { ServiceState::StartPending, ServiceState::Running, ServiceState::Stopped });
    auto const points = StartCheckpoints(*service.manager);
    CHECK(points.front() == 0);
    CHECK(std::ranges::is_sorted(points));
    CHECK(std::ranges::adjacent_find(points) == points.end());
    for (auto const& report: service.manager->Reports())
    {
        // Starting, a stop is accepted (R4-2) and a reload is not: it has nothing to act on yet.
        if (report.state == ServiceState::StartPending)
        {
            CHECK(report.acceptsStop);
            CHECK_FALSE(report.acceptsReload);
        }
        if (report.state == ServiceState::StartPending && report.checkPoint > 0)
            CHECK(report.waitHintMs == TestStartPlan.waitHint.count());
        if (report.state == ServiceState::Running)
        {
            CHECK(report.acceptsStop);
            CHECK(report.acceptsReload);
        }
    }
}

TEST_CASE("A start told to wait whose body returns before serving is reported stopped and never running",
          "[platform][service][refusal]")
{
    // The case that made the wait worth having: a body that refuses inside itself -- a port it cannot
    // bind -- must leave `net start` failing, so RUNNING must never have been said.
    ServingWhenReadyService service { TestStartPlan, DefaultDrainWait() };

    CHECK(service.host->Run([&service] {
        std::ignore = Testing::WaitUntil(
            "the start's first checkpoint",
            [&service] { return std::ranges::contains(StartCheckpoints(*service.manager), 1U); },
            [&service] { return std::format("{} report(s)", service.manager->Reports().size()); });
        return 78;
    }) == 78);

    CHECK(service.manager->Violations().empty());
    CHECK(Collapsed(service.manager->States()) == std::vector { ServiceState::StartPending, ServiceState::Stopped });
    CHECK(service.manager->Reports().back().exit
          == ServiceExit { .win32ExitCode = ServiceSpecificError, .serviceSpecificExitCode = 78 });
}

TEST_CASE("Past its ceiling a start's checkpoint stands still and a body that serves late is still reported running",
          "[platform][service]")
{
    // The ceiling bounds a WAITER, never the start: an installer's `net start` gives up one hint after
    // the checkpoint stops, and a body that serves after that must still reach RUNNING, or nothing could
    // ever stop it. On a stepped clock, so the ceiling is met at once and the count is exact.
    constexpr auto Ceiling = std::chrono::milliseconds { 10 };
    constexpr auto SleepsBeforeServing = 40;
    DaemonControls* controls = nullptr;
    Testing::SteppedDrainWait wait { [&controls, &wait] {
        if (wait.Sleeps() == SleepsBeforeServing && controls != nullptr)
            controls->MarkServing();
    } };
    ServingWhenReadyService service { StartPendingPlan { .waitHint = std::chrono::milliseconds { 9'000 },
                                                         .checkpointEvery = std::chrono::milliseconds { 1 },
                                                         .ceiling = Ceiling },
                                      wait };
    controls = &service.controls;
    auto running = false;

    CHECK(service.host->Run([&service, &running] {
        running = Testing::WaitUntil(
            "RUNNING after the late serve",
            [&service] { return std::ranges::contains(service.manager->States(), ServiceState::Running); },
            [&service] { return std::format("{} report(s)", service.manager->Reports().size()); });
        return 0;
    }) == 0);

    CHECK(running);
    CHECK(service.manager->Violations().empty());
    CHECK(Collapsed(service.manager->States())
          == std::vector { ServiceState::StartPending, ServiceState::Running, ServiceState::Stopped });
    // One checkpoint per millisecond of the ceiling and none after it, though the reporter went on
    // polling until the body served.
    CHECK(StartCheckpoints(*service.manager).back() == Ceiling.count());
    CHECK(wait.Sleeps() >= SleepsBeforeServing);
}

TEST_CASE("A start told to wait accepts a stop, and the service ends stopped without ever running", "[platform][service]")
{
    // R4-2: a start that waits for its body to serve can be long -- a large disk tier opening,
    // consensus recovering -- and one that accepted nothing while it started could be stopped by no
    // `sc stop`, MSI ServiceControl or uninstall until it served. Sent the way they send it: the
    // manager decides by the STATE first, so while the start is pending a stop is sent and a reload
    // is refused `ERROR_SERVICE_CANNOT_ACCEPT_CTRL` (1061) by the manager itself -- never 1052,
    // which would mean the state allowed it and the report did not accept it.
    ServingWhenReadyService service { TestStartPlan, DefaultDrainWait() };
    std::expected<bool, std::uint32_t> reloadWhileStarting { std::unexpected { 0U } };
    std::expected<bool, std::uint32_t> stopWhileStarting { std::unexpected { 0U } };
    auto stopProgressed = false;

    CHECK(service.host->Run([&] {
        std::ignore = Testing::WaitUntil(
            "the start's checkpoint to reach 2",
            [&service] { return std::ranges::contains(StartCheckpoints(*service.manager), 2U); },
            [&service] { return std::format("{} StartPending report(s)", StartCheckpoints(*service.manager).size()); });
        reloadWhileStarting = service.manager->Request(ServiceControlRequest::ParamChange);
        stopWhileStarting = service.manager->Request(ServiceControlRequest::Stop);
        // The stop is under way while the body is still starting: its checkpoint advances.
        stopProgressed = Testing::WaitUntil(
            "the stop's checkpoint to reach 2",
            [&service] { return std::ranges::contains(StopCheckpoints(*service.manager), 2U); },
            [&service] { return std::format("{} StopPending report(s)", StopCheckpoints(*service.manager).size()); });
        // A body that sees the stop ends its start rather than serving.
        return service.controls.StopRequested() ? 0 : 1;
    }) == 0);

    CHECK(reloadWhileStarting
          == std::expected<bool, std::uint32_t> { std::unexpected { Testing::ScmError::ServiceCannotAcceptCtrl } });
    CHECK(stopWhileStarting == std::expected<bool, std::uint32_t> { true });
    CHECK(stopProgressed);
    CHECK(service.controls.StopRequested());
    CHECK(service.manager->Violations().empty());
    CHECK(Collapsed(service.manager->States())
          == std::vector { ServiceState::StartPending, ServiceState::StopPending, ServiceState::Stopped });
    CHECK(service.manager->Reports().back().exit == ServiceExit { .win32ExitCode = 0, .serviceSpecificExitCode = 0 });
}

TEST_CASE("A stop accepted while starting is never followed by RUNNING, even from a body that serves at that moment",
          "[platform][service]")
{
    // The race the stop opens: the start's reporter has seen the body serve and is about to say
    // RUNNING when the control handler reports STOP_PENDING. RUNNING after it would tell the
    // manager the service started over. Placed rather than waited for: on a stepped wait, the
    // reporter's third sleep is where the body serves AND the stop arrives, so its RUNNING comes
    // after the STOP_PENDING every time.
    constexpr auto SleepOfTheRace = 3;
    std::atomic<bool> raced { false };
    std::atomic<ServingWhenReadyService*> raceIn { nullptr };
    Testing::SteppedDrainWait wait { [&raced, &raceIn, &wait] {
        auto* const service = raceIn.load();
        if (service == nullptr || wait.Sleeps() != SleepOfTheRace || raced.exchange(true))
            return;
        service->controls.MarkServing();
        std::ignore = service->manager->Request(ServiceControlRequest::Stop);
    } };
    ServingWhenReadyService service { TestStartPlan, wait };
    raceIn.store(&service);
    auto stopped = false;

    CHECK(service.host->Run([&service, &stopped] {
        stopped = Testing::WaitUntil(
            "the stop to be requested",
            [&service] { return service.controls.StopRequested(); },
            [&service] { return std::format("{} report(s)", service.manager->Reports().size()); });
        return 0;
    }) == 0);

    CHECK(raced.load());
    CHECK(stopped);
    CHECK(service.manager->Violations().empty());
    CHECK_FALSE(std::ranges::contains(service.manager->States(), ServiceState::Running));
    CHECK(Collapsed(service.manager->States())
          == std::vector { ServiceState::StartPending, ServiceState::StopPending, ServiceState::Stopped });
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
            ServiceStatusReport { .state = ServiceState::Running, .waitHintMs = 0, .exit = {}, .acceptsStop = false });
        std::ignore = manager.RegisterHandler("x", [](ServiceControlRequest, std::uint32_t) { return true; });
        manager.SetStatus(ServiceStatusReport { .state = ServiceState::StopPending, .waitHintMs = 0, .checkPoint = 1 });
        manager.SetStatus(ServiceStatusReport { .state = ServiceState::StopPending, .waitHintMs = 0, .checkPoint = 1 });
        manager.SetStatus(
            ServiceStatusReport { .state = ServiceState::Stopped, .waitHintMs = 0, .exit = {}, .acceptsStop = false });
        manager.SetStatus(
            ServiceStatusReport { .state = ServiceState::StopPending, .waitHintMs = 0, .exit = {}, .acceptsStop = false });
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

    // And a start's checkpoint, which a start that waits for its body to serve advances.
    ScriptedServiceControlManager starting { ServiceManagerPresence::Present };
    CHECK(starting.Dispatch("x", [&starting] {
        std::ignore = starting.RegisterHandler("x", [](ServiceControlRequest, std::uint32_t) { return true; });
        starting.SetStatus(ServiceStatusReport { .state = ServiceState::StartPending, .waitHintMs = 0, .checkPoint = 2 });
        starting.SetStatus(ServiceStatusReport { .state = ServiceState::StartPending, .waitHintMs = 0, .checkPoint = 2 });
        starting.SetStatus(ServiceStatusReport { .state = ServiceState::Stopped, .waitHintMs = 0 });
    }) == DispatchOutcome::Dispatched);
    CHECK(starting.Violations() == std::vector<std::string> { "a start's checkpoint did not advance" });

    // And a control is decided by the last reported STATE first and its acceptance second, refused
    // by the manager itself with Windows' own code and never reaching the handler -- the rule the
    // stop-while-starting case above stands on. Every row of the documented table is pinned in the
    // case below; these are the transitions a host goes through.
    ScriptedServiceControlManager refusing { ServiceManagerPresence::Present };
    std::vector<std::expected<bool, std::uint32_t>> answers;
    auto handled = 0;
    auto const refusingDispatched = refusing.Dispatch("x", [&] {
        std::ignore = refusing.RegisterHandler("x", [&handled](ServiceControlRequest, std::uint32_t) {
            ++handled;
            return true;
        });
        answers.push_back(refusing.Request(ServiceControlRequest::Stop)); // nothing reported yet
        refusing.SetStatus(ServiceStatusReport { .state = ServiceState::StartPending, .waitHintMs = 0 });
        answers.push_back(refusing.Request(ServiceControlRequest::Stop)); // a start that accepts nothing
        refusing.SetStatus(ServiceStatusReport { .state = ServiceState::Running, .waitHintMs = 0, .acceptsStop = true });
        answers.push_back(refusing.Request(ServiceControlRequest::ParamChange)); // stop accepted, reload not
        answers.push_back(refusing.Request(ServiceControlRequest::Stop));
        // A stop under way accepts no second one, whatever its report claims.
        refusing.SetStatus(ServiceStatusReport {
            .state = ServiceState::StopPending, .waitHintMs = 0, .checkPoint = 1, .acceptsStop = true });
        answers.push_back(refusing.Request(ServiceControlRequest::Stop));
        refusing.SetStatus(ServiceStatusReport { .state = ServiceState::Stopped, .waitHintMs = 0 });
        answers.push_back(refusing.Request(ServiceControlRequest::Stop));
    });
    CHECK(refusingDispatched == DispatchOutcome::Dispatched);
    using Answer = std::expected<bool, std::uint32_t>;
    CHECK(answers
          == std::vector<Answer> {
              Answer { std::unexpected { Testing::ScmError::InvalidServiceControl } }, // nothing reported yet
              Answer { std::unexpected { Testing::ScmError::InvalidServiceControl } }, // a start accepting nothing
              Answer { std::unexpected { Testing::ScmError::InvalidServiceControl } }, // running, reload not accepted
              Answer { true },
              Answer { std::unexpected { Testing::ScmError::ServiceCannotAcceptCtrl } }, // stop pending
              Answer { std::unexpected { Testing::ScmError::ServiceNotActive } },        // stopped
          });
    CHECK(handled == 1);
    CHECK(refusing.Violations().empty());
}

TEST_CASE("The scripted service manager sends a control as the ControlService remarks table says",
          "[platform][service][fake]")
{
    // The fake is the instrument every service case stands on, so its decisions are pinned against
    // Windows' DOCUMENTED behaviour, spelled here as codes rather than read from the fake's own
    // table -- the remarks table of `ControlService`
    // (https://learn.microsoft.com/windows/win32/api/winsvc/nf-winsvc-controlservice):
    //   STOPPED        stop (c)  other (c)    (a) sent if accepted, else 1052
    //   STOP_PENDING   stop (b)  other (b)    (b) 1061
    //   START_PENDING  stop (a)  other (b)    (c) 1062
    //   RUNNING        stop (a)  other (a)
    // An interrogation is accepted by every service by default. SHUTDOWN and power broadcasts come
    // from the system, not from `ControlService`, and the table does not cover them: the fake gives
    // them the OTHER column, the narrower reading (round 11 review, I1).
    using Answer = std::expected<bool, std::uint32_t>;
    auto const sent = Answer { true };
    auto const invalid = Answer { std::unexpected { Testing::ScmError::InvalidServiceControl } };
    auto const cannot = Answer { std::unexpected { Testing::ScmError::ServiceCannotAcceptCtrl } };
    auto const inactive = Answer { std::unexpected { Testing::ScmError::ServiceNotActive } };

    constexpr auto Controls = std::array {
        ServiceControlRequest::Stop,        ServiceControlRequest::Shutdown,   ServiceControlRequest::ParamChange,
        ServiceControlRequest::Interrogate, ServiceControlRequest::PowerEvent,
    };
    struct Row
    {
        std::vector<ServiceState> path; ///< The states reported, ending in the one under test.
        std::array<Answer, std::tuple_size_v<decltype(Controls)>> want; ///< Per control, in `Controls` order.
        bool accepting;                                                 ///< Whether the last report accepts every control.
    };
    auto const rows = std::array {
        Row { .path = { ServiceState::StartPending }, .want = { sent, cannot, cannot, cannot, cannot }, .accepting = true },
        Row { .path = { ServiceState::StartPending },
              .want = { invalid, cannot, cannot, cannot, cannot },
              .accepting = false },
        Row { .path = { ServiceState::StartPending, ServiceState::Running },
              .want = { sent, sent, sent, sent, sent },
              .accepting = true },
        Row { .path = { ServiceState::StartPending, ServiceState::Running },
              .want = { invalid, invalid, invalid, sent, invalid },
              .accepting = false },
        Row { .path = { ServiceState::StartPending, ServiceState::StopPending },
              .want = { cannot, cannot, cannot, cannot, cannot },
              .accepting = true },
        Row { .path = { ServiceState::StartPending, ServiceState::StopPending },
              .want = { cannot, cannot, cannot, cannot, cannot },
              .accepting = false },
        Row { .path = { ServiceState::StartPending, ServiceState::Stopped },
              .want = { inactive, inactive, inactive, inactive, inactive },
              .accepting = true },
        Row { .path = { ServiceState::StartPending, ServiceState::Stopped },
              .want = { inactive, inactive, inactive, inactive, inactive },
              .accepting = false },
    };

    auto checked = std::size_t { 0 };
    for (auto const& row: rows)
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, Controls.size()))
        {
            auto const control = Controls.at(index);
            auto const& want = row.want.at(index);
            ScriptedServiceControlManager manager { ServiceManagerPresence::Present };
            auto answer = Answer { std::unexpected { 0U } };
            CHECK(manager.Dispatch("x", [&] {
                std::ignore = manager.RegisterHandler("x", [](ServiceControlRequest, std::uint32_t) { return true; });
                for (auto const state: row.path)
                    manager.SetStatus(ServiceStatusReport { .state = state,
                                                            .waitHintMs = 0,
                                                            .checkPoint = 1,
                                                            .acceptsStop = row.accepting,
                                                            .acceptsReload = row.accepting,
                                                            .acceptsPowerEvents = row.accepting });
                answer = manager.Request(control);
                if (row.path.back() != ServiceState::Stopped)
                    manager.SetStatus(ServiceStatusReport { .state = ServiceState::Stopped, .waitHintMs = 0 });
            }) == DispatchOutcome::Dispatched);
            CAPTURE(static_cast<int>(row.path.back()), row.accepting, static_cast<int>(control));
            CHECK(answer == want);
            CHECK(manager.Violations().empty());
            ++checked;
        }
    }
    CHECK(checked == rows.size() * Controls.size());
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
