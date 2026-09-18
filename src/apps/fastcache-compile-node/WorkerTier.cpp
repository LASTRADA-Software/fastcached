// SPDX-License-Identifier: Apache-2.0
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "NodeAnnounce.hpp"
#include "NodeIoLoop.hpp"
#include "WorkerTier.hpp"

#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Platform/DaemonControls.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <utility>

#include <CacheProtocol.hpp>

namespace FastCache::Node
{

namespace
{

    /// How often a worker tells the scheduler it is alive.
    ///
    /// Comfortably inside `WorkerRegistry::DefaultHeartbeatTimeout` (90 s), because the
    /// two errors are not symmetric: a heartbeat that arrives late costs this worker
    /// its place in the fleet until it re-registers, while one that arrives early costs
    /// a few bytes.

    /// How many heartbeats between unconditional toolchain sweeps.
    ///
    /// Every beat asks the recorded witnesses, which costs a handful of `stat` calls and
    /// spawns nothing. This is the slower cadence at which the machine is surveyed
    /// regardless -- the only way back from serving LESS than the machine has, since a
    /// witness-driven recheck can only notice what it is already watching (#238).
    ///
    /// 45 beats is about a quarter of an hour at the default interval: long enough that
    /// the survey's driver spawns are nothing against a machine's load, short enough that
    /// a reinstalled compiler rejoins the fleet without anybody restarting a service.
    constexpr std::uint64_t SweepEveryBeats = 45;

    /// Per-call send/recv ceiling on the heartbeat's own connection to the scheduler.
    ///
    /// Was ten seconds passed as BOTH the dial bound and the I/O bound, which is the
    /// collapse `Cc::DialEndpoint` used to make: ten seconds is a reasonable ceiling
    /// on an exchange and a very long time to wait for a TCP handshake.
    constexpr std::chrono::milliseconds HeartbeatIoTimeout { 10'000 };

    /// Tell `node-status` what a finished survey concluded.
    ///
    /// The rule this carries -- which state a served count implies -- lives in
    /// `ToolchainStateFor`, where a test can reach it.
    /// @param state Where the worker publishes.
    /// @param served How many toolchains this node now serves.
    /// @param discovered How many candidates the cheap half of the survey found.
    void PublishToolchains(NodeRuntimeState& state, std::size_t served, std::size_t discovered)
    {
        state.PublishToolchains(ToolchainReading { .state = ToolchainStateFor(served),
                                                   .served = static_cast<std::uint32_t>(served),
                                                   .discovered = static_cast<std::uint32_t>(discovered) });
    }

    /// Tell `node-status` whether this node is getting through to a scheduler.
    ///
    /// Counted from the REGISTRARS rather than from the round's outcome, because they answer
    /// different questions: `accepted` says *a scheduler took something just now*, and a
    /// non-empty `WorkerId()` says *this registrar is currently registered somewhere*. A
    /// worker whose scheduler has gone away keeps its ids and accepts nothing, which is
    /// exactly the state worth being able to see.
    /// @param state Where the worker publishes.
    /// @param clock Stamps the acceptance; must be the clock `Describe()` differences it
    ///        against, or the age it reports is the difference between two clocks.
    /// @param held Every registration this node is trying to hold.
    /// @param accepted How many entries a scheduler took this round.
    void PublishRegistration(NodeRuntimeState& state,
                             IClock const& clock,
                             std::vector<Cc::WorkerRegistrar> const& held,
                             std::size_t accepted)
    {
        auto const registered =
            std::ranges::count_if(held, [](Cc::WorkerRegistrar const& registrar) { return !registrar.WorkerId().empty(); });
        state.PublishRegistration(static_cast<std::uint32_t>(registered),
                                  static_cast<std::uint32_t>(held.size()),
                                  accepted > 0 ? std::optional { clock.Now() } : std::nullopt);
    }

    /// Claim this worker's private scratch root, or say why the node must not start.
    ///
    /// The root is claimed EXCLUSIVELY. It used to be `temp_directory_path() /
    /// "fastcache-compile-node"` with jobs numbered beneath it from a counter starting at 1
    /// in every process, so a second node on this host derived the identical `job-1` -- and
    /// `create_directories` succeeds on a directory that already exists, so it was told
    /// nothing (#279). One node's cleanup then removed the directory under the other's
    /// compile, or the two shared `tu.o` and one answered with the other's object.
    ///
    /// @param servesCompiles Whether the cheap survey found anything to compile with.
    ///        Asked of what was DISCOVERED, not of what is served: nothing is served yet at
    ///        this point (#365), and reading the served map here would claim no root on
    ///        every node and then fail every compile for want of one.
    /// @param claimant Takes the claim.
    /// @param base Where the candidate roots live.
    /// @param logger Where the outcome is announced.
    /// @param conditions Where whether the root can be mapped is answered, every way out that
    ///        does not refuse to start.
    /// @return The held claim; a NULL claim when there is nothing to compile with; or why
    ///         the node must refuse to start.
    [[nodiscard]] std::expected<std::unique_ptr<IScratchClaim>, std::string> ClaimScratchRoot(
        bool servesCompiles,
        IScratchClaimant& claimant,
        std::filesystem::path const& base,
        ILogger& logger,
        NodeConditions& conditions)
    {
        if (!servesCompiles)
        {
            // Not `Clear`: nothing was checked, because there is no root. A worker that later finds
            // a toolchain still runs on the root it would have claimed here -- none -- so this stays
            // what it is for the life of the process.
            conditions.NotEvaluated(NodeCondition::ScratchRootUnmappable,
                                    "this worker found nothing to compile with, so it claimed no scratch root");
            return std::unique_ptr<IScratchClaim> {};
        }

        auto claimed = claimant.Claim(base, DefaultMaxScratchRoots);
        if (!claimed.has_value())
        {
            // Named, and never a fallback to an unclaimed root. Carrying on without the
            // claim would reintroduce #279 on exactly the machines least able to
            // diagnose it, and would do so while every test passed.
            auto const& row = DescribeScratchClaimRefusal(claimed.error());
            return std::unexpected { std::format("{}: {}", row.name, row.remedy) };
        }

        auto claim = std::move(*claimed);
        if (claim->Reclaimed())
            // A root whose lock was free but whose contents were not: its owner died
            // without running its cleanup. The COUNTER for it is raised by the tier's
            // constructor, beside the rest of what the worker counts.
            logger.Logf(LogLevel::Warn,
                        "reclaimed the scratch root {} from a node that exited without cleaning up",
                        claim->Root().string());
        logger.Logf(LogLevel::Info, "scratch root {} claimed exclusively", claim->Root().string());

        // **A root no mapping rule can name is said ONCE, here, in front of the operator.**
        // The rule `WorkerSourceNameRule` builds has this root on its left-hand side, so a
        // root carrying a space or an `=` makes every one of them unspellable and every
        // dispatched object goes back to recording `<scratch>/job-N/<name>` -- silently,
        // per job, with nothing counting it (#810). A warning and not a refusal: such a
        // machine compiles perfectly well and only its dispatched objects' debug names
        // degrade.
        for (auto const& warning: Cc::ScratchRootMappingWarnings(claim->Root().string()))
            logger.Logf(LogLevel::Warn, "{}", warning);

        // And said again where somebody arriving later can ask for it (#1364), from the same
        // derivation as the lines above, so the two cannot name different flags. Latched: the root
        // is claimed once and held for the life of the process.
        auto const unmappable = Cc::ScratchRootUnmappableFlags(claim->Root().string());
        if (unmappable.empty())
            conditions.Clear(NodeCondition::ScratchRootUnmappable);
        else
            conditions.Raise(NodeCondition::ScratchRootUnmappable,
                             ListDetail(std::format("this worker's scratch root {} cannot be spelled inside a rule of:",
                                                    claim->Root().string()),
                                        std::vector<std::string> { unmappable.begin(), unmappable.end() }));
        return claim;
    }

    /// What the runner needs of a served set: the compiler per fingerprint.
    /// @param served The served toolchains.
    /// @return The projection.
    [[nodiscard]] std::map<std::string, std::string> CompilersOf(std::map<std::string, ServedToolchain> const& served)
    {
        std::map<std::string, std::string> compilers;
        for (auto const& [fingerprint, toolchain]: served)
            compilers.emplace(fingerprint, toolchain.compiler);
        return compilers;
    }

    /// Adopt a reloaded compile-argument allowlist, and say so when it changed.
    /// @param jobs The runner the set is applied to.
    /// @param logger Where the change is announced, at Warn.
    /// @param inForce The set applied now; replaced when the candidate differs.
    /// @param candidate The set the live configuration names.
    void AdoptAllowlist(Cc::CompileJobRunner& jobs,
                        ILogger& logger,
                        std::vector<std::string>& inForce,
                        std::vector<std::string> const& candidate)
    {
        auto const said = AllowlistAnnouncement(AllowlistMoment::Reload, inForce, candidate);
        if (!said)
            return;

        inForce = candidate;
        jobs.ReplaceExtraAllowedArgs(inForce);
        logger.Log(LogLevel::Warn, *said);
    }

} // namespace

WorkerMachine MakeSystemWorkerMachine()
{
    auto runner = Cc::MakeProcessRunner();
    auto host = Cc::MakeToolchainHost();
    // Built UNCONDITIONALLY and consulted per call, since #403 made
    // `--no-toolchain-discovery` reloadable. It costs nothing to hold: it stores two
    // references and searches only when asked.
    auto discovery = Cc::MakeToolchainDiscovery(*host, *runner);
    return WorkerMachine { .runner = std::move(runner),
                           .host = std::move(host),
                           .discovery = std::move(discovery),
                           .claimant = MakeLockFileScratchClaimant(),
                           .scratchBase = ScratchBaseDirectory() };
}

std::expected<std::unique_ptr<WorkerTier>, std::string> WorkerTier::Start(WorkerTierParts const& parts,
                                                                          WorkerMachineFactory const& makeMachine)
{
    // Through `WorkerSlotsOf`, never `OfferableSlots` directly -- its header says which
    // way the other spelling fails (#206). Absent is a node running no worker, and it
    // gets no tier at all.
    auto const slots = WorkerSlotsOf(parts.cfg, parts.capacity);
    if (!slots.has_value())
        return std::unique_ptr<WorkerTier> {};

    // The link the heartbeat announces through, built HERE so a worker cannot exist
    // without one: the tier's constructor takes it by value.
    auto link = SchedulerLink::For(parts.cfg.schedulers);
    if (!link.has_value())
        return std::unexpected { std::string {
            "a worker was started with no --scheduler, which the startup table exists to refuse: it would never "
            "register, never be leased and never be sent a job. Name the scheduler's --listen-node endpoint" } };

    // Only now, with a worker decided: see `MakeSystemWorkerMachine`.
    auto machine = makeMachine();

    // Only the CHEAP half of the survey runs here, and that split is the whole of #365.
    // Deciding WHICH compilers to serve is a spawn per candidate; IDENTIFYING them walks
    // every byte under every include root, measured at 5136 files and over 300 s on a
    // cold Windows runner (#354). Discovery stays on the startup path because it is what
    // refuses a misconfigured node promptly and by name; the walk runs on the heartbeat
    // thread, and nothing registers until it answers.
    //
    // `discovery` is consulted per call against the configuration it is HANDED, since
    // #403 made `--no-toolchain-discovery` reloadable: an object built only when the
    // flag was off at startup is one the flag could never be turned back on for.
    auto* const discovery = parts.cfg.toolchainDiscovery ? machine.discovery.get() : nullptr;
    auto discovered = DiscoverToolchainEntries(parts.cfg, discovery, *machine.runner, parts.logger);
    if (!discovered.has_value())
        // The entry was named above, at Error, by the survey that could not read it.
        return std::unexpected { std::string { "a malformed --toolchain was named" } };

    auto claim = ClaimScratchRoot(
        !discovered->entries.empty(), *machine.claimant, machine.scratchBase, parts.logger, parts.conditions);
    if (!claim.has_value())
        return std::unexpected { std::move(claim).error() };

    // What this worker keeps between lease checks: the grants it has already run (#614),
    // the scheduler term the last authentic grant named (#421), and where a term going
    // BACKWARDS is reported -- at Warn, because the term is a diagnostic rather than a
    // gate since #614. Written by nothing but the validator, which borrows it for the
    // tier's life; a heap object so that borrow survives the tier being built around it.
    auto leaseState = std::make_unique<Distributed::WorkerLeaseState>(Distributed::SchedulerTermRegressionNotice {
        [&logger = parts.logger](std::string_view line) { logger.Logf(LogLevel::Warn, "{}", line); } });

    // The whole trust decision is one call, made and announced where a test can reach
    // it: a grant carries an HMAC over this worker's endpoint, the toolchain, the key
    // and an expiry, so the check is local and costs the job nothing. A WALL clock,
    // because the expiry was stamped on another machine.
    //
    // It borrows `main`'s one `AnnouncedEndpoint` -- see `WorkerTierParts::announced` for why
    // there is exactly one per process rather than one per component that needs an address.
    auto validator = MakeWorkerLeaseValidator(
        parts.cfg, parts.announced, parts.activation, DefaultSystemWallClock(), *leaseState, parts.metrics, parts.logger);
    if (!validator.has_value())
        return std::unexpected { std::move(validator).error() };

    return std::unique_ptr<WorkerTier> { new WorkerTier(parts,
                                                        std::move(machine),
                                                        *std::move(discovered),
                                                        *std::move(claim),
                                                        std::move(leaseState),
                                                        *std::move(validator),
                                                        *std::move(link),
                                                        *slots) };
}

WorkerTier::WorkerTier(WorkerTierParts const& parts,
                       WorkerMachine machine,
                       DiscoveredToolchains discovered,
                       std::unique_ptr<IScratchClaim> scratchClaim,
                       std::unique_ptr<Distributed::WorkerLeaseState> leaseState,
                       Cc::LeaseValidator validator,
                       SchedulerLink link,
                       std::uint32_t slots):
    _cfg { parts.cfg },
    _reloader { parts.reloader },
    _cacheTier { parts.cacheTier },
    _credential { parts.credential },
    _proofKey { parts.proofKey },
    _metrics { parts.metrics },
    _logger { parts.logger },
    _announced { parts.announced },
    _machine { std::move(machine) },
    _discovered { std::move(discovered) },
    _scratchClaim { std::move(scratchClaim) },
    // Constructed BEFORE anything has been identified, and told so. An empty map and an
    // unanswered one are opposite answers to a client, so until the survey lands a job
    // is refused `ToolchainSurveyInFlight` rather than `UnknownFingerprint` (#365).
    _jobs { *_machine.runner,
            _scratchClaim != nullptr ? _scratchClaim->Root() : _machine.scratchBase,
            CompilersOf(_toolchains),
            Cc::ToolchainSurvey::InFlight() },
    _appliedExtraArgs { parts.cfg.extraAllowedArgs },
    _leaseState { std::move(leaseState) },
    // The envelope ceiling is THIS surface's request cap, named rather than left to the
    // decoder's default. `AvailableCodecs()`, never a literal: this list is what the
    // worker answers a compile in, chosen against what the client accepts (#265).
    _protocol { _jobs, std::move(validator), Cc::AvailableCodecs(), parts.metrics, WorkerMaxRequestBytes },
    _slots { slots },
    // Sized to the slot cap, which is what makes an admitted job always find a thread,
    // and declared before the capacity and the responder, so neither outlives it.
    _pool { slots },
    _capacity { slots, WorkerMaxRequestBytes, parts.cfg.drainTimeout, parts.logger },
    // The compile verbs on the node's own `0xFC` listener. A frame arrives on the reactor;
    // the compile leaves it for the pool and the reply comes BACK to it, which is why
    // `CompileResponder_test.cpp` asserts the thread identities (#213, #290).
    _responder { _protocol, _capacity,          parts.membership, parts.locality,
                 _pool,     parts.io.Reactor(), parts.metrics,    parts.logger },
    // Seeded with the honest first reading: the cheap half has run and the walk has not,
    // so this node is `Surveying` 0 of however many candidates there are.
    _runtime { ToolchainReading { .state = CompileCacheWire::ToolchainState::Surveying,
                                  .served = 0,
                                  .discovered = static_cast<std::uint32_t>(_discovered.entries.size()) } },
    _advertisedWire { Distributed::CapacityToWire(parts.capacity) },
    _registrarNotice { [&logger =
                            parts.logger](std::string_view text) { logger.Logf(LogLevel::Warn, "scheduler: {}", text); } },
    _dialer { HeartbeatIoTimeout },
    _link { std::move(link) }
{
    // Counted as well as logged because it is otherwise visible nowhere: a rise means
    // nodes are dying rather than stopping.
    if (_scratchClaim != nullptr && _scratchClaim->Reclaimed())
        _metrics.Increment(IMetricsSink::Counter::WorkerScratchRootsReclaimed);

    // The operator's additions to the compile-argument allowlist, applied before the
    // surface opens so no job is ever judged by a half-applied set -- and announced at
    // WARN, the level a credential change is logged at, because this is the one setting
    // that widens what a client may make this worker's compiler do (#293).
    _jobs.ReplaceExtraAllowedArgs(_appliedExtraArgs);
    if (auto const said = AllowlistAnnouncement(AllowlistMoment::Startup, {}, _appliedExtraArgs))
        _logger.Log(LogLevel::Warn, *said);

    // Compiled in, never configurable: the column this feeds tells an operator which
    // binary is running on each machine, mid-upgrade most often. And the hostname, a
    // LABEL that decides nothing (#1024). Both ride the capacity record because it is
    // REGISTER's one extensible field.
    _advertisedWire.version = VersionString;
    _advertisedWire.displayName = parts.host.Facts().hostName;
}

std::vector<Cc::WorkerRegistrar> WorkerTier::RegistrarsFor(std::map<std::string, ServedToolchain> const& served)
{
    // One registrar per toolchain, because REGISTER carries ONE fingerprint and
    // `--toolchain` is repeatable. Every entry heartbeats the SAME machine-wide in-flight
    // count, so the scheduler stops picking all of them together once this worker is busy.
    //
    // The endpoint is asked ONCE for the whole set rather than per registrar: every entry here
    // describes the same machine at the same moment, and a publish landing between two
    // of them would register one toolchain at the new address and the rest at the old.
    auto const advertised = _announced.Current();

    std::vector<Cc::WorkerRegistrar> built;
    built.reserve(served.size());
    for (auto const& [fingerprint, toolchain]: served)
    {
        // A copy per registrar: the label is the one field of this record that is NOT
        // node-wide (#194).
        auto perToolchain = _advertisedWire;
        perToolchain.toolchainLabel = toolchain.label;
        built.emplace_back(_registrarNotice, fingerprint, advertised, _slots, Cc::AvailableCodecs(), perToolchain);
    }
    return built;
}

void WorkerTier::Serve(std::map<std::string, ServedToolchain> served)
{
    _toolchains = std::move(served);
    // The compile port first, the registration second: between the two this worker
    // refuses a job naming a dropped fingerprint rather than serving it with the new
    // compiler, and it never announces a fingerprint it is not yet ready to serve.
    _jobs.ReplaceToolchains(CompilersOf(_toolchains));
    AdoptRegistrars(RegistrarsFor(_toolchains), _registrars, _withdrawals);
    // AFTER the two calls above: this says the worker is serving, and it must not say so
    // while the compile port still holds the previous answer.
    PublishToolchains(_runtime, _toolchains.size(), _discovered.entries.size());
}

void WorkerTier::AnnounceAs(std::string endpoint)
{
    // Published FIRST, so the registrars built below carry the new address and the lease
    // check moves in the same step. The old registrars still hold the address they
    // registered under -- `Cc::WorkerRegistrar` keeps its own endpoint precisely so a
    // withdrawal names the entry that exists rather than the one about to.
    _announced.Publish(std::move(endpoint));

    // The compile port is untouched, and that is the difference from `Serve`: the
    // toolchains and the compilers behind them are unchanged, so nothing about what this
    // worker will RUN moves. Only the address it is filed under does.
    AdoptRegistrars(RegistrarsFor(_toolchains), _registrars, _withdrawals);

    // Republished for `node-status`, because the registered count drops to zero until the
    // round that follows re-registers -- and a status still claiming those toolchains
    // registered would be describing entries that were just withdrawn.
    PublishToolchains(_runtime, _toolchains.size(), _discovered.entries.size());
}

WorkerHeartbeat WorkerTier::Launch(IClock const& statusClock)
{
    return WorkerHeartbeat { std::jthread {
        [this, &statusClock](std::stop_token const& stop) { Heartbeat(stop, statusClock); } } };
}

void WorkerTier::Heartbeat(std::stop_token const& stop, IClock const& statusClock)
{
    // One sampler for the whole loop, not one per heartbeat: CPU utilization is a
    // difference between two readings, so a sampler per beat would report nothing,
    // forever. Configured with the scratch path, whose filesystem this worker writes to.
    auto const loadSampler = MakeHostLoadSampler(MakeSystemCounterSource(_jobs.ScratchRoot()));

    HeartbeatRound const round { .cfg = _cfg,
                                 .registrars = _registrars,
                                 .withdrawals = _withdrawals,
                                 .capacity = _capacity,
                                 .loadSampler = *loadSampler,
                                 .cacheTier = _cacheTier,
                                 .metrics = _metrics,
                                 .credential = _credential,
                                 .notice = _registrarNotice,
                                 .proofKey = _proofKey,
                                 // The RESOLVED id, which `AdoptNodeIdentity` stamped into the
                                 // running configuration -- so a node that minted one presents
                                 // the same label the cluster knows it by, and one that runs no
                                 // consensus presents an empty label, which is legal.
                                 .nodeId = _cfg.nodeId,
                                 .lease = *_leaseState,
                                 .fleetMismatch = _fleetAssertionFailed,
                                 .logger = _logger };

    // The configuration snapshot this thread last surveyed against, so a reload is
    // noticed by COMPARISON rather than by a flag somebody else sets. Seeded BEFORE the
    // initial survey, which is what makes the first beat comparable to every other one.
    // Null only when this worker has no configuration file at all.
    auto actedOn = _reloader != nullptr ? _reloader->Current() : nullptr;

    // The initial survey, HERE rather than at startup because it is the expensive half
    // (#354): off the startup path the node has already bound its port and said it is
    // ready, so a machine that takes minutes to identify its toolchains spends them
    // SERVING. The stop token is passed because a SIGTERM arriving mid-walk is caught,
    // and an unobservable walk would make the process wait it out before stopping.
    auto surveyed = FingerprintToolchains(_discovered, *_machine.runner, *_machine.host, _toolchainClock, _logger, stop);
    switch (surveyed.outcome)
    {
        case SurveyOutcome::Served:
            Serve(std::move(surveyed.served));
            break;
        case SurveyOutcome::NothingToServe:
            // "Nothing to serve" was a startup refusal before #365 and stays one, only
            // later: left running, such a worker registers nothing and reports a healthy
            // ready line. Published first, so an operator asking what is wrong in the
            // window before the stop lands gets a real answer.
            PublishToolchains(_runtime, 0, _discovered.entries.size());
            _surveyFoundNothing = true;
            DaemonControls::Instance().RequestStop();
            return;
        case SurveyOutcome::NoneCouldBeAsked:
            // **Not fatal** (#1060): every candidate left because it could not be ASKED,
            // so this node is not misconfigured. Falls through to the loop, whose
            // periodic sweep surveys again, exactly as it does for a machine that loses
            // its last compiler while serving.
            PublishToolchains(_runtime, 0, _discovered.entries.size());
            break;
        case SurveyOutcome::Cancelled:
            // Already stopping, and interrupted rather than misconfigured.
            return;
    }

    std::uint64_t beat = 0;
    while (!stop.stop_requested())
    {
        ++beat;

        // The configuration this beat acts on, read once and held for the whole beat:
        // `Current()` can be swapped by a reload mid-beat.
        auto const snapshot = _reloader != nullptr ? _reloader->Current() : nullptr;
        auto const& liveCfg = snapshot ? *snapshot : _cfg;

        auto const reloaded = ClaimsReloadedBetween(actedOn, snapshot);
        actedOn = snapshot;

        // Compared SEPARATELY from `reloaded`: extending the allowlist changes nothing
        // this worker advertises, so gating it on that answer would be a reload an
        // operator watched do nothing.
        AdoptAllowlist(_jobs, _logger, _appliedExtraArgs, liveCfg.extraAllowedArgs);

        // Compared SEPARATELY from `reloaded` as well, and for the opposite half of that
        // reason: this changes what the fleet must be TOLD while changing nothing about
        // the toolchains, so it must not ride the re-survey `reloaded` triggers -- an
        // include-tree walk to move a string would be minutes of work telling the fleet
        // nothing it could not have had at once. The DERIVED endpoint is what is
        // compared; `AdvertisedEndpointChange` owns why.
        //
        // At Warn, beside the allowlist's: an address change is a fleet-visible event an
        // operator is watching for, and the one thing that explains a burst of
        // `LeaseEndpointMismatch` in the minutes after it.
        if (auto moved = AdvertisedEndpointChange(_announced.Current(), snapshot))
        {
            _logger.Log(LogLevel::Warn, moved->announcement);
            AnnounceAs(std::move(moved->endpoint));
        }
        auto const depth = RecheckDepthFor(reloaded, beat, SweepEveryBeats);
        auto const voice = SurveyVoiceFor(reloaded, depth);

        auto* const discovery = liveCfg.toolchainDiscovery ? _machine.discovery.get() : nullptr;
        if (auto refreshed = RefreshToolchains(
                _toolchains, liveCfg, discovery, *_machine.runner, *_machine.host, _toolchainClock, _logger, depth, voice);
            refreshed.changed)
        {
            Serve(std::move(refreshed.served));

            // A worker that ends up serving nothing keeps running and says so rather than
            // exiting: the compiler may come back with the next package, and a routine
            // upgrade must not remove a machine from the fleet permanently.
            if (_toolchains.empty())
                _logger.Logf(LogLevel::Warn, "this machine now has no usable toolchain; serving nothing until one returns");
        }

        // Read BEFORE the round, so a cordon arriving while it runs is one the wake below
        // still sees as a change.
        auto const announcedCordon = _capacity.IsCordoned();
        PublishRegistration(_runtime, statusClock, _registrars, AnnounceRound(round, _link, _dialer));

        // A cordon, or its lifting, reaches the scheduler at once rather than a whole
        // interval later (#1303); a stop ends the wait immediately.
        if (_capacity.WaitForHeartbeat(stop, announcedCordon, NodeAnnounceInterval) == HeartbeatWake::Stopped)
            break;
    }
}

void WorkerTier::StopAndDrain()
{
    // Both doors closed and every compile drained while the node's reactor is still
    // turning: a compile admitted through the merged surface finishes on the pool and then
    // hops back onto the reactor to hand back its reply, so tearing the surface down first
    // can stop the reactor with a compile still out. Destruction order cannot express
    // this, because the surface points at the responder and the responder at this
    // capacity, which is why the drain is callable rather than only a destructor.
    _capacity.BeginShutdown();
    _capacity.Drain();
}

} // namespace FastCache::Node
