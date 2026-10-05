// SPDX-License-Identifier: Apache-2.0
#include "FormationEffects.hpp"
#include "FormationLoop.hpp"
#include "NodeRefusal.hpp"

#include <FastCache/Platform/ProcessExit.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <format>
#include <utility>

namespace FastCache::Node
{

void ReformRequest::RequestReform()
{
    _requested.store(true, std::memory_order_release);
}

bool ReformRequest::Pending() const noexcept
{
    return _requested.load(std::memory_order_acquire);
}

bool ReformRequest::Take() noexcept
{
    return _requested.exchange(false, std::memory_order_acq_rel);
}

std::string_view DrainSentence(bool reforming) noexcept
{
    return reforming
               ? "formation: this node's shape changed; no longer accepting compiles, and starting again from its record"
               : "stop requested; no longer accepting compiles";
}

BodyOutcome ClassifyBodyEnd(int exitCode, ReformRequest& reform, bool stopRequested) noexcept
{
    // Taken whatever the answer, so a flag raised by a body that then refused or was stopped does not
    // turn the NEXT body's first stop into a reform.
    auto const reformed = reform.Take();
    if (exitCode != ExitCodeOf(ProcessExit::Served))
        return BodyOutcome { .end = BodyEnd::Refused, .exitCode = exitCode };
    if (reformed && !stopRequested)
        return BodyOutcome { .end = BodyEnd::Reform, .exitCode = exitCode };
    return BodyOutcome { .end = BodyEnd::Stopped, .exitCode = exitCode };
}

namespace
{
    /// The tail every adoption shares: finish an interrupted archive, then shape @p cfg.
    /// @param cfg The configuration to shape.
    /// @param record The record read, or minted.
    /// @param remembered The fleet endpoints last known.
    /// @param store Where the record is kept.
    /// @param archiver What moves a left cluster's store out of the root.
    /// @return The record and the remembered endpoints, or why the node must not start.
    [[nodiscard]] std::expected<AdoptedFormation, std::string> ResumeAndApply(NodeConfig& cfg,
                                                                              Cluster::FormationRecord record,
                                                                              Cluster::FleetEndpoints remembered,
                                                                              Cluster::IFormationStore& store,
                                                                              IStoreArchiver& archiver)
    {
        // Before any consensus tier opens the directory: the ONLY place a store is moved, at the start
        // after a crash and at every reform after a move.
        auto resumed = ResumeFormation(std::move(record), store, archiver);
        if (!resumed.has_value())
            return std::unexpected { std::move(resumed).error() };
        if (auto applied = ApplyFormation(cfg, *resumed, remembered); !applied.has_value())
            return std::unexpected { std::move(applied).error() };
        return AdoptedFormation { .record = *std::move(resumed), .remembered = std::move(remembered) };
    }
} // namespace

std::expected<AdoptedFormation, std::string> AdoptFormation(NodeConfig& cfg,
                                                            Cluster::IFormationStore& store,
                                                            IStoreArchiver& archiver,
                                                            Cluster::FleetEndpointsFile& endpoints,
                                                            ISecureRandom& random,
                                                            core::platform::IWallClock const& wall)
{
    auto kept = ReadKeptFormation(store, endpoints);
    if (!kept.has_value())
        return std::unexpected { std::move(kept).error() };
    auto record = KeepFormation(*kept, store, random, wall);
    if (!record.has_value())
        return std::unexpected { std::move(record).error() };
    return ResumeAndApply(cfg, *std::move(record), std::move(kept->remembered), store, archiver);
}

std::expected<AdoptedFormation, std::string> ReadoptFormation(NodeConfig& cfg,
                                                              Cluster::IFormationStore& store,
                                                              IStoreArchiver& archiver,
                                                              Cluster::FleetEndpointsFile& endpoints)
{
    auto kept = ReadKeptFormation(store, endpoints);
    if (!kept.has_value())
        return std::unexpected { std::move(kept).error() };
    if (!kept->record.has_value())
        return std::unexpected { std::string { FormationRecordGone } };
    return ResumeAndApply(cfg, *std::move(kept->record), std::move(kept->remembered), store, archiver);
}

ReformPace NextReformPace(std::size_t run, core::platform::SteadyTimePoint::duration served) noexcept
{
    auto const next = served >= ReformRunEnds ? std::size_t { 1 } : run + 1;
    return ReformPace { .run = next, .wait = ReformBackoff.at(std::min(next, ReformBackoff.size()) - 1) };
}

std::expected<NodeConfig, std::string> AdoptForReform(NodeConfig next,
                                                      AdoptedFormation& running,
                                                      ReformAdoption const& parts)
{
    auto adopted = ReadoptFormation(next, parts.store, parts.archiver, parts.endpoints);
    if (!adopted.has_value())
        return std::unexpected { std::move(adopted).error() };
    // Judged as a restart would judge it: a shape the startup rules refuse must not be served by a
    // reform and then refused at the next restart, with every reload between declined.
    if (auto const rejection = StartupPolicyRejection(next); rejection.has_value())
        return std::unexpected { std::format("the shape this node moved to is refused at startup: {}", *rejection) };
    running = *std::move(adopted);
    // What a reload compares against, and what the worker reads where it registers from, is the node as
    // it now runs -- not as it started.
    if (parts.publisher != nullptr)
        parts.publisher->Publish(next);
    return next;
}

int RunFormationLoop(INodeConfigSource const& config,
                     ServingBody const& body,
                     FormationAdopter const& adopt,
                     LoopControls const& controls,
                     ILogger& logger)
{
    // How many reforms in a row, each after a body that served less than `ReformRunEnds`.
    auto run = std::size_t { 0 };
    while (true)
    {
        // What is in force NOW, read per body: a reform shaped from the start's configuration would
        // publish it over every accepted reload.
        auto shaped = adopt(config.Current());
        if (!shaped.has_value())
        {
            logger.Logf(LogLevel::Error, "{}; refusing to start", shaped.error());
            return ExitCodeFor(StartStage::Formation);
        }
        auto const began = controls.clock.now();
        auto const outcome = body(*shaped);
        if (outcome.end != BodyEnd::Reform)
            return outcome.exitCode;
        logger.Logf(LogLevel::Info,
                    "formation: this node left the shape {} ran in; starting again from its record",
                    DescribeFormationMode(*shaped));

        auto const pace = NextReformPace(run, controls.clock.now() - began);
        run = pace.run;
        auto const wait = pace.wait;
        if (wait > std::chrono::milliseconds::zero())
        {
            logger.Logf(LogLevel::Warn,
                        "formation: {} reforms in a row, each after a body that served less than {}; waiting {} "
                        "before the next",
                        run,
                        ReformRunEnds,
                        std::chrono::duration_cast<std::chrono::seconds>(wait));
            controls.pause(wait);
        }
        // Asked again here, after the body's own end was classified: a stop that arrived while the
        // body drained, or during the wait, starts no further body -- which would bind every port and
        // start consensus only to drain them again.
        if (controls.stopRequested())
        {
            logger.Log(LogLevel::Info, "formation: a stop arrived between two bodies; starting none");
            return outcome.exitCode;
        }
    }
}

int RunNodeBodies(INodeConfigSource const& config,
                  AdoptedFormation& running,
                  NodeBodies const& parts,
                  NodeBody const& body,
                  ILogger& logger)
{
    auto first = true;
    return RunFormationLoop(
        config,
        [&](NodeConfig const& shaped) {
            // Asked per body: a body's listener closes what it adopts, so each serves a copy of its own.
            auto served = parts.activation.ForBody();
            if (!served.has_value())
            {
                logger.Logf(LogLevel::Error, "{}; refusing to start", served.error());
                return BodyOutcome { .end = BodyEnd::Refused,
                                     .exitCode = ExitCodeFor(NodeRefusalCause::HandedOverListeners) };
            }
            auto const exitCode = body(shaped, running.record, *served);
            return ClassifyBodyEnd(exitCode, parts.reform, parts.controls.stopRequested());
        },
        [&](NodeConfig next) -> std::expected<NodeConfig, std::string> {
            // The start adopted the first body's record itself, where a refusal could still be reported.
            if (std::exchange(first, false))
                return next;
            return AdoptForReform(std::move(next), running, parts.adoption);
        },
        parts.controls,
        logger);
}

KeptFormationReader RunningFormationOf(AdoptedFormation const& running)
{
    return [&running] {
        return std::expected<KeptFormation, std::string> { KeptFormation { .record = running.record,
                                                                           .remembered = running.remembered } };
    };
}

void ReloaderPublisher::Publish(NodeConfig const& cfg)
{
    _reloader.Publish(cfg);
}

ActivationHold::ActivationHold(std::optional<int> inherited, IInheritedDescriptors const& descriptors) noexcept:
    _inherited { inherited },
    _descriptors { descriptors }
{
}

ActivationHold::~ActivationHold()
{
    if (_inherited.has_value())
        _descriptors.Close(*_inherited);
}

std::expected<std::optional<int>, std::string> ActivationHold::ForBody() const
{
    if (!_inherited.has_value())
        return std::optional<int> {};
    return _descriptors.Duplicate(*_inherited).transform([](int copy) { return std::optional { copy }; });
}

} // namespace FastCache::Node
