// SPDX-License-Identifier: Apache-2.0
///
/// `fastcache-compile-node` — a compile worker for fastcached's distributed
/// execution.
///
/// Registers with a scheduler, then answers `Compile` requests from clients the
/// scheduler sent. It is not a cache and not a scheduler: it holds no keys, stores
/// nothing, and is given no cache credentials. The object it produces goes back to
/// the client, which stores it — see `Cc::Dispatch` for why that is the trust model
/// rather than an accident of layering.
///
#include "AdminEndpoint.hpp"
#include "CacheTier.hpp"
#include "ClusterAdminCli.hpp"
#include "CompileCapacity.hpp"
#include "CompileResponder.hpp"
#include "ConsensusTier.hpp"
#include "DiscoveryTier.hpp"
#include "NodeAnnounce.hpp"
#include "NodeConfig.hpp"
#include "NodeCredential.hpp"
#include "NodeFrameSurface.hpp"
#include "NodeIdentity.hpp"
#include "NodeIoLoop.hpp"
#include "NodeLogging.hpp"
#include "NodeMembership.hpp"
#include "NodeReload.hpp"
#include "NodeStatusResponder.hpp"
#include "NodeSurfaces.hpp"
#include "NodeToolchains.hpp"
#include "SchedulerLink.hpp"
#include "SchedulerTier.hpp"
#include "ScratchClaim.hpp"
#include "WorkerLease.hpp"

#include <FastCache/Async/Task.hpp>
#include <FastCache/Async/ThreadPoolExecutor.hpp>
#include <FastCache/Cache/InMemoryLruStorage.hpp>
#include <FastCache/Cli/Options.hpp>
#include <FastCache/Cli/UsageDoc.hpp>
#include <FastCache/Config/ByteSize.hpp>
#include <FastCache/Config/ConfigReloader.hpp>
#include <FastCache/Config/DefaultConfigPath.hpp>
#include <FastCache/Config/FileOptions.hpp>
#include <FastCache/Config/SecretExposureWatcher.hpp>
#include <FastCache/Config/SecretProvenance.hpp>
#include <FastCache/Config/YamlReader.hpp>
#include <FastCache/Core/HostPort.hpp>
#include <FastCache/Core/Logger.hpp>
#include <FastCache/Core/ReadinessMarker.hpp>
#include <FastCache/Core/Version.hpp>
#include <FastCache/Distributed/FleetView.hpp>
#include <FastCache/Distributed/LeaseToken.hpp>
#include <FastCache/Distributed/SchedulerProtocol.hpp>
#include <FastCache/Metrics/IMetricsSink.hpp>
#include <FastCache/Metrics/PrometheusFormatter.hpp>
#include <FastCache/Net/BlockingSocket.hpp>
#include <FastCache/Platform/CpuAffinity.hpp>
#include <FastCache/Platform/DaemonControls.hpp>
#include <FastCache/Platform/Environment.hpp>
#include <FastCache/Platform/HostInfo.hpp>
#include <FastCache/Platform/HostLoad.hpp>
#include <FastCache/Platform/HostMemory.hpp>
#include <FastCache/Platform/IDaemonHost.hpp>
#include <FastCache/Platform/InheritedListener.hpp>
#include <FastCache/Platform/LocalAddresses.hpp>
#include <FastCache/Platform/NarrowText.hpp>
#include <FastCache/Platform/Terminal.hpp>
#include <FastCache/Platform/WindowsEventLogger.hpp>
#include <FastCache/Protocol/CompileCacheWire.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CompileJob.hpp>
#include <Dispatch.hpp>
#include <EndpointDial.hpp>
#include <ToolchainDiscovery.hpp>
#include <ToolchainHost.hpp>
#include <WorkerProtocol.hpp>

namespace
{
using namespace FastCache;
using namespace FastCache::Node;

/// How often a worker tells the scheduler it is alive.
///
/// Comfortably inside `WorkerRegistry::DefaultHeartbeatTimeout` (90 s), because the
/// two errors are not symmetric: a heartbeat that arrives late costs this worker
/// its place in the fleet until it re-registers, while one that arrives early costs
/// a few bytes.
constexpr std::chrono::seconds HeartbeatInterval { 20 };

/// Slices the heartbeat sleep is broken into, so a stop request is observed
/// promptly rather than after a full interval.
constexpr int HeartbeatSlices = 20;

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

/// Ceiling on OPENING that connection, name resolution included.
constexpr std::chrono::milliseconds HeartbeatConnectTimeout { 1'000 };

/// How often the stop watcher looks at the stop flag.
///
/// A signal handler may portably do almost nothing -- it sets a flag -- so
/// something else has to notice and close the listener, and it cannot be the
/// accept loop, which is parked inside `Accept()`. Short enough that `systemctl
/// stop` and Ctrl-C feel immediate, long enough to cost nothing while idle.
constexpr std::chrono::milliseconds StopPollInterval { 100 };

/// Record a stop request.
///
/// `extern "C"` and doing nothing but setting the process-wide flag, because a
/// signal handler is not allowed to do more: `DaemonControls`' flag is atomic and
/// lock-free, which is what makes this legal where logging or allocating here
/// would not be.
extern "C" void HandleNodeStopSignal(int /*signum*/)
{
    DaemonControls::Instance().RequestStop();
}

/// Record a reload request.
///
/// Same constraint as the stop handler: a signal handler sets a lock-free flag and
/// does nothing else. The re-read, the immutability check and the logging all happen
/// on the thread that notices the flag.
extern "C" void HandleNodeReloadSignal(int /*signum*/)
{
    DaemonControls::Instance().RequestReload();
}

/// Ask for a graceful stop, and for a reload, on the signals a supervisor sends.
///
/// **SIGHUP used to be deliberately unhandled, and the argument for that is now the
/// argument for the `reloadable` column.** It read: *this worker has nothing it could
/// reload -- its toolchain table is what its registration advertised, so re-reading it
/// would leave the scheduler dispatching against a set this worker no longer serves.*
///
/// The premise stopped being true when [#291](https://github.com/LASTRADA-Software/fastcached/issues/291)
/// gave the node a configuration file. **The reasoning did not**, and it is exactly why
/// reloadability is a column of `NodeOptions()` rather than a property of the reload
/// path: those fields must refuse to change, for precisely the reason that paragraph
/// gives. Restated rather than deleted, because a correct argument attached to a false
/// premise is worth more than either half
/// ([#292](https://github.com/LASTRADA-Software/fastcached/issues/292)).
///
/// So SIGHUP is handled, and **what it can change is decided by the table** --
/// `NodeOptions()`'s `reloadable` column, which is the one place that answer lives.
///
/// This sentence used to finish *"today only `--log-level`"*, and that was a second
/// source of truth for a fact the table already held. It had gone false: the column
/// marks several rows now, among them `--requirepass` and the flags that decide who
/// this node admits. So a reader who believed the comment thought a SIGHUP could not
/// rotate this node's credential or change its admission policy, when it can do both
/// -- which is the wrong direction to be wrong in, and exactly the reading somebody
/// reaches for while deciding whether a reload is safe to send.
///
/// **No corrected count replaces it.** A restated figure drifts again, and the reason
/// to name one here was never good: whoever needs the set can read the column, and
/// whoever needs it to be RIGHT needs it in one place rather than two that agree
/// today. `--log-level` is still the field an operator reaches for mid-incident and
/// the one `ILogger` can be told about while running -- an example, not the set.
void InstallNodeStopHandlers()
{
    std::signal(SIGINT, &HandleNodeStopSignal);
    std::signal(SIGTERM, &HandleNodeStopSignal);
#if !defined(_WIN32)
    // POSIX only: Windows has no SIGHUP, and a service there is reconfigured through
    // the SCM control handler rather than by a signal.
    std::signal(SIGHUP, &HandleNodeReloadSignal);
#endif
}

/// The descriptor a supervisor handed this worker, when there was one.
///
/// Separated from `main` so the whole handoff -- how many descriptors arrived, and
/// whether the configuration can still describe this worker afterwards -- is one
/// decision with one answer, rather than three checks interleaved with everything
/// else a startup does.
///
/// **A descriptor rather than a listener**, since #290 stage 3. The merged `0xFC`
/// surface runs on the reactor, and the listener that serves it is built by
/// `FrameEndpoint::StartAdopted` from this descriptor; building one here would own
/// it, and handing an owned descriptor on is a double close rather than a handover.
///
/// Nothing here closes what it returns. On the two refusal paths the process exits
/// immediately, and on the success path ownership passes to the listener -- which
/// takes it even when the adoption itself fails.
/// @param cfg What the operator asked for.
/// @param logger Where the handoff is announced.
/// @return The descriptor, `std::nullopt` when nothing was handed over, or why the
///         handoff cannot be served.
[[nodiscard]] std::expected<std::optional<int>, std::string> ActivatedDescriptor(NodeConfig const& cfg, ILogger& logger)
{
    // When a supervisor already bound the port and handed the descriptor over,
    // binding it again would fail with "address already in use" -- against
    // ourselves. Falling through to the ordinary bind when nothing was handed over
    // is what lets one binary serve both a `.socket` unit and a plain
    // `--listen-node`, with no flag distinguishing them: the environment says which,
    // and it says so unambiguously.
    auto const inherited = AdoptInheritedDescriptors();
    if (inherited.empty())
        return std::optional<int> {};

    // Only the first would be used. This worker answers one protocol on one port, so
    // a unit listing several sockets is a misconfiguration -- reported rather than
    // half-honoured, since silently ignoring the rest would leave an operator with a
    // port that accepts nothing and no clue why.
    if (inherited.size() > 1)
        return std::unexpected { std::format("socket activation handed over {} listeners; this worker serves exactly one",
                                             inherited.size()) };

    // Socket activation makes --advertise mandatory, because the fallback becomes a
    // guess the process cannot make. `--listen-node` was not used -- the socket unit
    // chose the address and this process is never told which -- so the fallback would
    // register a value from configuration that describes nothing, and the wildcard is
    // not an address a remote client can dial anyway.
    //
    // The consequence of guessing is the worst-shaped failure this system has: the
    // registration SUCCEEDS, the worker heartbeats happily, the scheduler leases that
    // endpoint to clients, and every one of them fails to connect and compiles
    // locally. Nothing reports an error, and the fleet looks healthy from both ends.
    // Refusing at startup, where it can be explained, is the whole difference.
    if (cfg.advertise.empty())
        return std::unexpected { std::string {
            "--advertise is required under socket activation: the socket unit owns the port, so this worker "
            "cannot know what address clients should use" } };

    logger.Logf(LogLevel::Info, "a supervisor handed over a listening socket; --listen-node is not used");
    return std::optional { inherited.front() };
}

/// What `main` returns when the operator's configuration is wrong.
///
/// Every refusal in this program is that same fact -- a flag missing, a flag that
/// cannot be honoured, an endpoint that will not bind -- so it is one name rather
/// than a `2` spelled seven times, and distinct from the `1` a supervisor reads as
/// "it ran and then died".
constexpr int ExitUsage = 2;

/// What `main` returns when the worker served until it was asked to stop.
constexpr int ExitOk = 0;

/// Announce this machine once, following `NotLeader` to wherever it points.
///
/// A function rather than a block inside `WorkerBody` for two reasons, and the
/// second is the load-bearing one. `WorkerBody` is at the cognitive-complexity
/// ceiling the build enforces -- this loop pushed it to 88 against a threshold of
/// 60, which is the linter making a design point rather than a style one. And a
/// redirect chain with a memory and a fallback is far too much behaviour to leave
/// in the one translation unit no test reaches.
///
/// Every *decision* still belongs to `SchedulerLink`, which is pure and tested:
/// which endpoint, whether the chain is spent, whether to fall back, what to
/// remember. What lives here is the dialling, and the logging of what the link
/// decided.
///
/// @param round What to announce and where to read it from.
/// @param link Where this node believes the leader is; advanced across the round.
/// @param connector Dials each endpoint the link names.
/// @return How many entries a scheduler ACCEPTED this round. Zero covers every way a
///         round can achieve nothing -- nobody reachable, everybody refusing, a
///         redirect chain that ran out -- which are one answer to the only question
///         the caller asks of it: is this node getting through to a scheduler.
[[nodiscard]] std::size_t AnnounceRound(Node::HeartbeatRound const& round,
                                        Node::SchedulerLink& link,
                                        BlockingConnector& connector)
{
    for (link.BeginRound();;)
    {
        auto client =
            Cc::DialEndpointBlocking(connector, link.Target(), DialOptions { .connectTimeout = HeartbeatConnectTimeout });
        if (client == nullptr)
        {
            round.logger.Logf(LogLevel::Warn,
                              "scheduler {} unreachable{}",
                              link.Target(),
                              link.Following() ? "; falling back to the configured endpoint" : "");
            // A remembered leader that stopped answering is retried against the
            // configured endpoint now rather than a heartbeat interval from now:
            // this machine is out of the fleet for as long as it takes, and the
            // configured endpoint is the one still standing after an election the
            // remembered leader lost.
            if (!link.Lost().has_value())
                return 0;
            continue;
        }

        auto const outcome = Node::AnnounceOnce(round, *client, link.Target());
        if (!outcome.leader.has_value())
        {
            // Committed only when this endpoint actually took an entry. It answered
            // either way, but an endpoint that refused every registrar for its own
            // reasons -- not a member, a fingerprint it will not have -- is not a
            // leader worth starting the next round at, and pinning to it would
            // outlast the election that caused it.
            if (outcome.accepted > 0)
            {
                link.Accepted();
                return outcome.accepted;
            }
            if (!link.Lost().has_value())
                return 0;
            continue;
        }

        round.logger.Logf(
            LogLevel::Info, "scheduler {} is not the leader; announcing to {} instead", link.Target(), *outcome.leader);
        if (!link.Redirect(*outcome.leader))
        {
            // Two schedulers naming each other, or a leader that moved again
            // mid-chain. Costs this round rather than the thread.
            round.logger.Logf(LogLevel::Warn,
                              "gave up following leader redirects after {} hop(s); retrying next heartbeat",
                              Node::MaxAnnounceRedirects);
            return 0;
        }
    }
}

/// Tell `node-status` what a finished survey concluded.
///
/// A free function rather than a lambda inside `WorkerBody` for `AnnounceRound`'s
/// reason, which is build-enforced rather than stylistic: that body sits at the
/// cognitive-complexity ceiling the linter fails the build on, and two publication
/// closures pushed it over. The rule this carries -- which state a served count implies
/// -- lives in `ToolchainStateFor`, where a test can reach it.
///
/// @param state Where the worker publishes.
/// @param served How many toolchains this node now serves.
/// @param discovered How many candidates the cheap half of the survey found.
void PublishToolchains(Node::NodeRuntimeState& state, std::size_t served, std::size_t discovered)
{
    state.PublishToolchains(Node::ToolchainReading { .state = Node::ToolchainStateFor(served),
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
///
/// @param state Where the worker publishes.
/// @param clock Stamps the acceptance; must be the clock `Describe()` differences it
///        against, or the age it reports is the difference between two clocks.
/// @param held Every registration this node is trying to hold.
/// @param accepted How many entries a scheduler took this round.
void PublishRegistration(Node::NodeRuntimeState& state,
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
/// A function rather than a block inside `WorkerBody` because it is a startup
/// decision with its own vocabulary -- and because `WorkerBody` is already at the
/// cognitive-complexity ceiling the build enforces, which is the honest reason a
/// reader deserves rather than a silenced warning.
///
/// @param servesCompiles Whether this node runs a worker tier at all.
/// @param base Where the candidate roots live.
/// @param logger Where the outcome is announced.
/// @return The held claim; a NULL claim when there is no worker tier and none is
///         needed; or nothing at all when the node must refuse to start.
[[nodiscard]] std::optional<std::unique_ptr<Node::IScratchClaim>> ClaimWorkerScratchRoot(bool servesCompiles,
                                                                                         std::filesystem::path const& base,
                                                                                         ILogger& logger)
{
    if (!servesCompiles)
        return std::unique_ptr<Node::IScratchClaim> {};

    auto const claimant = Node::MakeLockFileScratchClaimant();
    auto claimed = claimant->Claim(base, Node::DefaultMaxScratchRoots);
    if (!claimed.has_value())
    {
        // Named, and never a fallback to an unclaimed root. Carrying on without the
        // claim would reintroduce #279 on exactly the machines least able to
        // diagnose it, and would do so while every test passed.
        auto const& row = Node::DescribeScratchClaimRefusal(claimed.error());
        logger.Logf(LogLevel::Error, "{}: {}; refusing to start", row.name, row.remedy);
        return std::nullopt;
    }

    auto claim = std::move(*claimed);
    if (claim->Reclaimed())
        // A root whose lock was free but whose contents were not: its owner died
        // without running its cleanup. The COUNTER for it is raised by the caller,
        // where the metrics sink exists -- the claim has to happen before that
        // because the job runner takes the root at construction.
        logger.Logf(LogLevel::Warn,
                    "reclaimed the scratch root {} from a node that exited without cleaning up",
                    claim->Root().string());
    logger.Logf(LogLevel::Info, "scratch root {} claimed exclusively", claim->Root().string());

    // **A root no mapping rule can name is said ONCE, here, in front of the operator.**
    // The rule `WorkerSourceNameRule` builds has this root on its left-hand side, so a
    // root carrying a space or an `=` makes every one of them unspellable and every
    // dispatched object goes back to recording `<scratch>/job-N/<name>` -- silently,
    // per job, with nothing counting it (#810). The root is chosen once, so the
    // question is answered once: a startup property decided per request is the shape
    // this repository already records for the worker's lease check.
    //
    // A warning and not a refusal: such a machine compiles perfectly well and only its
    // dispatched objects' debug names degrade. The sentences are built by a pure
    // function so what they SAY is testable; this file is in no test target.
    for (auto const& warning: Cc::ScratchRootMappingWarnings(claim->Root().string()))
        logger.Logf(LogLevel::Warn, "{}", warning);
    return claim;
}

/// Serve until asked to stop.
///
/// Everything `main` does once it has decided this process is going to BE a
/// worker. Separated so it can be handed to an `IDaemonHost`, which double-forks
/// on POSIX or hands control to the Windows SCM -- neither of which can wrap a
/// `main` that has already parsed, validated and registered.
///
/// The cheap, fallible checks stay in `main` deliberately: run in here they would
/// print their diagnosis to a stdout the POSIX host has already redirected to
/// /dev/null, so a misconfigured worker would exit in silence.
/// @param cfg What this worker was told to be.
/// @param logger Where it reports.
/// @return Process exit code.
/// The node's reloader, named once in `NodeReload.hpp` so that the policies reading
/// the live snapshot -- `Node::ReloadedCredential` among them -- do not each respell
/// `ConfigReloaderOf<NodeConfig>`.
using Node::NodeReloader;

/// Resolve this node's identity and put it into both configurations.
///
/// A function rather than a block in `main` for the reason `StartCacheTierOrExplain`
/// is one: it is a coherent decision with one answer, and inline it pushed `main` past
/// clang-tidy's cognitive-complexity limit. What it decides is `NodeIdentity`'s and not
/// this file's -- `main.cpp` is in no test target, so a rule spelled here is one
/// nothing can assert.
///
/// BOTH configurations, because they are used for two different things and each is
/// wrong without it: @p cfg is what this process runs with and what the startup table
/// judges, and @p cliOnly is what a service registration bakes in.
/// @param cfg The merged configuration, completed in place.
/// @param cliOnly The command-line-only parse a registration is built from.
/// @param random Where a minted identity's bits come from.
/// @param logger Where the identity, or the refusal, is reported.
/// @return The identity, DISENGAGED when this invocation needs none, or why it could
///         not be resolved. `std::expected` over a nested optional, because "no
///         identity is wanted here" and "one was wanted and could not be had" are the
///         two states a caller acts on differently and two optionals render alike.
[[nodiscard]] std::expected<std::optional<Node::NodeIdentity>, std::string> AdoptNodeIdentity(NodeConfig& cfg,
                                                                                              NodeConfig& cliOnly,
                                                                                              IRandomSource& random,
                                                                                              ILogger& logger)
{
    if (Node::NodeIdentityNeed(cfg) != Node::IdentityNeed::Mint)
        return std::optional<Node::NodeIdentity> {};

    auto resolved = Node::ResolveNodeIdentity(Node::NodeStateDirectory(cfg), cfg.nodeId, random);
    if (!resolved.has_value())
        return std::unexpected { std::move(resolved).error() };

    Node::ApplyNodeIdentity(cfg, *resolved);
    Node::ApplyNodeIdentity(cliOnly, *resolved);

    // Which of the three it was, said out loud once. "This node kept the identity it
    // had" and "this node invented one" are the pair that matters and the pair a silent
    // start cannot tell apart -- the second is a machine the cluster has never
    // admitted, and an operator who sees it after a restart has lost a state directory.
    logger.Logf(LogLevel::Info, "node identity {} ({})", resolved->id, Node::DescribeNodeIdentityOrigin(resolved->origin));
    return std::optional<Node::NodeIdentity> { *std::move(resolved) };
}

/// Adopt the compile-argument allowlist an accepted reload asks for, and say so when
/// it moved.
///
/// A function rather than four lines inside the heartbeat loop. `WorkerBody` is the
/// one place in this binary where a branch is charged twice -- once to
/// `readability-function-cognitive-complexity`, which this pushed over its threshold,
/// and once to whoever next has to read the loop whole.
///
/// The DECISION is not here: `AllowlistAnnouncement` owns whether anything is said and
/// what, in a file the test target builds. What is left here is applying it, and the
/// level -- WARN, because this set decides what a client may make the compiler do.
/// @param jobs Where the set takes effect.
/// @param logger Where the change is announced.
/// @param inForce The set currently applied; replaced when it moves.
/// @param candidate What the reloaded configuration asks for.
void AdoptAllowlist(Cc::CompileJobRunner& jobs,
                    ILogger& logger,
                    std::vector<std::string>& inForce,
                    std::vector<std::string> const& candidate)
{
    auto const said = Node::AllowlistAnnouncement(Node::AllowlistMoment::Reload, inForce, candidate);
    if (!said)
        return;

    inForce = candidate;
    jobs.ReplaceExtraAllowedArgs(inForce);
    logger.Log(LogLevel::Warn, *said);
}

[[nodiscard]] int WorkerBody(NodeConfig const& cfg, ILogger& logger, NodeReloader* reloader)
{
    // ONE origin for every uptime this process reports. `/healthz` and the `0xFC`
    // `NodeStatus` verb both answer *how long has this been serving*, and two
    // independently sampled origins are two numbers that can disagree about one fact --
    // an operator comparing a scrape against the CLI would be reading a difference that
    // describes nothing. Taken as the first statement, so it is this body's own start
    // rather than whichever startup step happened to be declared above the reader.
    auto const startedAt = std::chrono::steady_clock::now();

    // Socket activation is resolved BEFORE the toolchains, and the order is
    // deliberate. Computing a fingerprint walks the whole include tree and takes
    // seconds; a bad handoff is decided in microseconds. Doing the cheap, fallible
    // thing first means a misconfigured unit fails immediately instead of after a
    // multi-second pause -- and it means the startup log reads in the order things
    // actually happened, so an operator watching a worker come up sees what it did
    // with the socket before the long quiet part.
    auto const activatedOrError = ActivatedDescriptor(cfg, logger);
    if (!activatedOrError.has_value())
    {
        logger.Logf(LogLevel::Error, "{}", activatedOrError.error());
        return ExitUsage;
    }
    auto const activated = *activatedOrError;

    auto const runner = Cc::MakeProcessRunner();

    auto const toolchainHost = Cc::MakeToolchainHost();
    // Built UNCONDITIONALLY, and gated per call instead, since #403 made
    // `--no-toolchain-discovery` reloadable. A discovery object constructed only when
    // the flag was off at startup is one the flag can never be turned back on for --
    // the running worker would hold a null it could not refill, so a reload that
    // enabled discovery would silently keep serving nothing.
    //
    // It costs nothing to hold: `MakeToolchainDiscovery` stores two references and
    // searches only when `Discover()` is called. `discoveryFor` is what decides,
    // and it reads the configuration it is HANDED rather than the startup one, so
    // the reloaded snapshot governs on the heartbeat thread.
    auto const discovery = Cc::MakeToolchainDiscovery(*toolchainHost, *runner);
    auto const discoveryFor = [&discovery](NodeConfig const& against) -> Cc::IToolchainDiscovery* {
        return against.toolchainDiscovery ? discovery.get() : nullptr;
    };

    // The ONE derivation, shared with the startup refusal that judges it. This value
    // goes to `MakeWorkerLeaseValidator` below and to the heartbeat's REGISTER, and a
    // lease's MAC is taken over exactly this string -- so the endpoint the scheduler
    // signs, the endpoint this worker verifies and the endpoint clients dial are one
    // fact with one author. See `AdvertisedEndpoint`.
    auto const advertise = Node::AdvertisedEndpoint(cfg);

    // The descriptor travels to `StartNodeSurfaceOrExplain` below and is served
    // there. It used to be refused here, for the two months between the surfaces
    // merging and the reactor listeners learning to adopt: `AdoptInheritedListeners`
    // handed back a BLOCKING listener, the merged surface runs on the reactor, and
    // there was nothing that could join the two. #464 added `PlatformListener::Adopt`
    // and this is the last stitch of #290 stage 3 closing over it.
    //
    // The interim was a refusal rather than an adoption left unserved, and that shape
    // is worth keeping in mind for the next such gap: a packaged Linux install enables
    // the worker THROUGH the socket unit -- the `.service` deliberately has no
    // `[Install]` section -- so a node that took the descriptor and then answered
    // nothing on it would have been the silent failure this whole ticket exists to
    // remove, on the deployment path most people use.

    // Surveyed HERE rather than before the port is bound, and the order is the one
    // socket activation already argues for a few lines up: do the cheap, fallible
    // thing first. Binding costs microseconds and fails on a port another process
    // holds; the survey reads every byte under every include root and has been
    // measured exceeding 300 s on a cold Windows runner (#354). Surveyed first, a
    // node with a port conflict walked its whole toolchain and only then said the
    // address was taken.
    //
    // Only the CHEAP half runs here, and that split is the whole of #365. Deciding
    // WHICH compilers to serve is a spawn per candidate; IDENTIFYING them walks every
    // byte under every include root, measured at 5136 files and over 300 s on a cold
    // Windows runner (#354). Done here, that walk is time during which this node
    // serves nothing at all -- not its cache tier, not `/healthz`, not `/metrics`.
    //
    // So discovery stays on the startup path, because it is what refuses a
    // misconfigured node promptly and by name, and the fingerprinting moves to the
    // heartbeat thread's first round, below the tiers.
    //
    // Registration is NOT moved with it, and cannot be: `toolchains` stays empty
    // until the walk answers, `registrarsFor` builds nothing from an empty map, and a
    // `ServedToolchain` cannot exist without a real fingerprint. So this node
    // advertises nothing until it knows what it is -- which is #365's acceptance
    // criterion, and #225 is what happens when it is not met.
    //
    // Its own clock rather than one of the tiers': it is what the hash phase's
    // progress rate reads elapsed time from.
    SteadyClock const toolchainClock;
    auto discoveredOrNone = Node::DiscoverToolchainEntries(cfg, discoveryFor(cfg), *runner, logger);
    if (!discoveredOrNone.has_value())
        return ExitUsage;
    auto const discoveredToolchains = *std::move(discoveredOrNone);

    // Empty until the heartbeat thread's first round answers. NOT const for the same
    // reason it never was: a compiler patched under a running service makes it stale
    // and the heartbeat re-derives it (#238) -- the initial survey is now simply the
    // first of those.
    std::map<std::string, Node::ServedToolchain> toolchains;

    // The scratch root is claimed EXCLUSIVELY, and it is the worker tier's alone.
    //
    // It used to be `temp_directory_path() / "fastcache-compile-node"` with jobs
    // numbered beneath it from a counter starting at 1 in every process, so a second
    // node on this host derived the identical `job-1` -- and `create_directories`
    // succeeds on a directory that already exists, so it was told nothing (#279).
    // One node's cleanup then removed the directory under the other's compile, or
    // the two shared `tu.o` and one answered with the other's object.
    //
    // `servesCompiles` rather than an unconditional claim: a machine scheduling for
    // a fleet and compiling nothing has no use for a scratch root and must not fail
    // to start for want of one. Every node serves compiles today, so this is always
    // true -- named anyway, so that when a node can offer zero slots (#206) the
    // claim is already conditional rather than something somebody has to remember.
    //
    // Asked of what was DISCOVERED, not of what is served: since #365 nothing is
    // served yet at this point, and reading the served map here would have claimed no
    // scratch root on every node -- then failed every compile for want of one, once
    // the survey answered and jobs started arriving. The two maps are equal in size
    // only after the walk, which is exactly what has not happened yet.
    auto const servesCompiles = !discoveredToolchains.entries.empty();
    auto const scratchBase = Node::ScratchBaseDirectory();
    auto scratchClaimOrRefusal = ClaimWorkerScratchRoot(servesCompiles, scratchBase, logger);
    if (!scratchClaimOrRefusal.has_value())
        return ExitUsage;
    auto const scratchClaim = std::move(*scratchClaimOrRefusal);
    auto const scratch = scratchClaim ? scratchClaim->Root() : scratchBase;
    // Projected to what the runner needs, by a lambda rather than in place, because
    // the re-survey below builds the identical map from a different set of
    // toolchains -- and a projection written twice is one that drifts.
    auto compilersOf = [](std::map<std::string, Node::ServedToolchain> const& served) {
        std::map<std::string, std::string> compilers;
        for (auto const& [fingerprint, toolchain]: served)
            compilers.emplace(fingerprint, toolchain.compiler);
        return compilers;
    };
    // Constructed BEFORE anything has been identified, and told so. An empty map and
    // an unanswered one are the same `std::map` and opposite answers to a client, so
    // the state is carried beside it rather than inferred from it: until the survey
    // lands, a job is refused `ToolchainSurveyInFlight` -- "this worker is starting"
    // -- instead of `UnknownFingerprint`, which says the fleet is matching the wrong
    // machines and sends an operator to look at the wrong thing (#365).
    Cc::CompileJobRunner jobs { *runner, scratch, compilersOf(toolchains), Cc::ToolchainSurvey::InFlight() };

    // The operator's additions to the compile-argument allowlist, applied before the
    // surface opens so no job is ever judged by a half-applied set.
    //
    // **Announced at WARN, which is the level a credential change is logged at.** This
    // is the one setting that widens what a client may make this worker's compiler do:
    // every entry is an argument the built-in table refused to recognise. An operator
    // reading a log after an incident has to be able to see that the set was extended
    // and to what, so silence here is not an option even though the usual level for a
    // configuration line is INFO (#293).
    //
    // WHAT to say is `Node::AllowlistAnnouncement`, a pure function in a file the test
    // target builds, for `RecheckDepthFor`'s reason: this one is in none (#909). All
    // that is left here is the level and the moment.
    auto appliedExtraArgs = cfg.extraAllowedArgs;
    jobs.ReplaceExtraAllowedArgs(appliedExtraArgs);
    if (auto const said = Node::AllowlistAnnouncement(Node::AllowlistMoment::Startup, {}, appliedExtraArgs))
        logger.Log(LogLevel::Warn, *said);

    // **A node that works for other machines and opens no admin surface is told once,
    // at INFO** (#1304). `--admin-listen` is off unless asked for, so such a node has
    // no `/healthz`, no `/metrics` and no dashboard: everything works and nothing off
    // this machine can see it. The default is deliberately NOT flipped -- binding a
    // port nobody asked for is the decision this binary refuses everywhere.
    //
    // WHO it applies to and WHAT it says are `Node::ObservabilityAnnouncement`'s, for
    // the reason the line above delegates: this file is in no test target (#909), so a
    // rule written here could only ever be checked by reading it. INFO rather than the
    // Warn beside it because nothing was widened and nothing is wrong.
    if (auto const said = Node::ObservabilityAnnouncement(cfg))
        logger.Log(LogLevel::Info, *said);

    // A lease is CHECKED, and by a validator built about a hundred lines below --
    // `MakeWorkerLeaseValidator`, which verifies the grant's MAC, the endpoint it
    // names, and spends it once (#281, #614). This comment used to open "every lease
    // is accepted", which was true when it was written and was contradicted three
    // sentences later by its own account of the signed credential; a reader arriving
    // at the sentence rather than the paragraph took the worker for an open door.
    //
    // What is still reachability-plus-membership is the surface in FRONT of the
    // lease: who may open a connection at all. That is the same boundary the cache
    // tier has, and the inbound credential that would replace it is #976.
    AtomicMetricsSink metrics;
    // Raised here rather than at the claim above, which runs before this sink exists.
    // Counted as well as logged because it is otherwise visible nowhere: a rise means
    // nodes are dying rather than stopping.
    if (scratchClaim != nullptr && scratchClaim->Reclaimed())
        metrics.Increment(IMetricsSink::Counter::WorkerScratchRootsReclaimed);
    // One policy for all THREE surfaces -- the compile port here, the scheduler and
    // the cache below -- and it outlives every one of them. A node that answered "is
    // this peer one of ours" differently at two of its surfaces would admit a peer to
    // the fleet and refuse it the objects that fleet produced, or worse, the reverse.
    // Not `const`: consensus republishes the member set into it while the node runs,
    // which is the whole point of membership being a replicated log entry rather than
    // a command-line list.
    Node::NodeMembership membership { cfg, logger };

    // The lease a client presents is CHECKED here, and membership is not a substitute
    // for it. `WorkerServer` gates the port on the member oracle, so what reaches
    // this protocol is this machine or a peer the operator admitted -- but "admitted
    // to the fleet" is not "granted this compile", and for as long as those were the
    // same answer any admitted machine could spend any worker's CPU without ever
    // asking the scheduler for a slot (#282).
    //
    // No round trip: a grant carries an HMAC over this worker's endpoint, the
    // toolchain, the key and an expiry, signed with `--cluster-key-file` -- which
    // this node already reads for discovery (#281, `Distributed/LeaseToken.hpp`). So
    // the check is a local `VerifyLeaseToken` and costs the job nothing.
    //
    // The residual, deliberately: a lease is checked, not SPENT. Nothing here tells
    // the scheduler the slot was taken, and a client that mints one request and sends
    // it twice compiles twice against one grant. Inside a fleet that has agreed a key
    // that is a fairness question rather than a security one -- the machine is busier
    // than the scheduler believes, which the heartbeat corrects within one interval.
    //
    // The envelope ceiling is THIS surface's request cap, named rather than left to
    // the decoder's default: `WorkerProtocol` never sees the listener that enforced
    // the frame length, so a copy of the figure on each side is two literals that
    // have to agree forever, and lowering one would silently stop bounding the other.
    //
    // `AvailableCodecs()`, not a literal `{ Identity }`. This list is what the worker
    // answers a compile in, chosen against what the client said it accepts -- so a
    // literal here is a node that can never compress a reply however both ends are
    // built, which is what it was, and every dispatched object crossed the network
    // uncompressed (#265). It is the client's own list computed by the client's own
    // function, because the two ends of a negotiation deriving it separately is how
    // they come to disagree.

    // The key the validator verifies with is the same `--cluster-key-file` that
    // discovery and the scheduler read. Absent means a node admitting only its own
    // machine -- `StartupPolicyRejection` refuses every other shape -- so by the time
    // this runs the decision has been made once and announced, rather than being
    // taken per request where nothing would ever say it had been.
    // The whole trust decision is one call, made and announced where a test can
    // reach it. `main` is the one translation unit that cannot be unit-tested, so it
    // holds none of the policy -- see `MakeWorkerLeaseValidator`.
    //
    // A **wall** clock, because the expiry it checks was stamped on another machine
    // and a steady instant means nothing off the host that read it. The process
    // singleton rather than a local: the validator borrows it for the rest of this
    // node's life, and `DefaultSystemWallClock()` is the lifetime that argument
    // wants -- a local here was one more object whose outliving had to be reasoned
    // about, for no gain.
    // What this worker keeps between lease checks: the grants it has already run
    // (#614), the scheduler term the last authentic grant named (#421), and where a
    // term going BACKWARDS is reported.
    //
    // ONE object rather than three locals, because the validator below borrows all
    // three for the rest of this node's life and three separate locals is three chances
    // to declare two and dangle on the third. Declared here for that lifetime.
    //
    // Written by nothing but the validator itself: a grant that has passed its MAC
    // teaches the term inside it, and since #614 it must also have been unspent. #421's
    // first shape also had the heartbeat reply state the term, and review found that
    // channel is unauthenticated -- anything able to answer this node's `--scheduler`
    // dial could push the expectation to `UINT64_MAX` and make this worker refuse every
    // honest grant until it restarted. So the channel was deleted rather than defended,
    // which is what `CacheResponder` taking no membership oracle already records as this
    // repository's preference.
    //
    // Empty at this point, and both halves of that are states rather than gaps. A worker
    // that has learned no term reports none, which is what keeps its first grant from
    // reading as a scheduler reset; and a restart is what empties the spent set, which
    // reopens a captured grant's window for whatever is left of its expiry -- stated on
    // `Distributed::SpentLeases` rather than left to be discovered, and the same window
    // the term expectation has always had across a restart.
    //
    // `Warn` rather than `Error` on the reset line: the worker is healthy and goes on
    // compiling, because the term is a diagnostic rather than a gate since #614. What
    // may have changed under it is the operator's own configuration, which is exactly
    // what one line is for.
    Distributed::WorkerLeaseState leaseState { Distributed::SchedulerTermRegressionNotice {
        [&logger](std::string_view line) { logger.Logf(LogLevel::Warn, "{}", line); } } };

    auto validator =
        Node::MakeWorkerLeaseValidator(cfg,
                                       advertise,
                                       activated.has_value() ? Node::SocketActivation::Yes : Node::SocketActivation::No,
                                       DefaultSystemWallClock(),
                                       leaseState,
                                       metrics,
                                       logger);
    if (!validator.has_value())
    {
        logger.Logf(LogLevel::Error, "{}", validator.error());
        return ExitUsage;
    }

    Cc::WorkerProtocol protocol { jobs, *std::move(validator), Cc::AvailableCodecs(), metrics, WorkerMaxRequestBytes };

    // The worker server and the admin endpoint are both built BELOW the cache tier,
    // and in both cases moving them down was the fix rather than tidying: one takes
    // a slot count that is not knowable until the tier has been built or not been
    // (#167), and the other's snapshot lambda has to report the node's cache -- so
    // captured up here it could only ever say `.storage = std::nullopt`, which is
    // exactly what it said, comment and all, for as long as this program has had a
    // cache.

    // The fleet scheduler, when this node is the one running it. Off unless asked
    // for: handing out other machines' CPU time is an operator's decision, not
    // something they get by starting a worker.
    //
    // The three objects have to outlive the endpoint, which holds references into
    // them, so they are declared here rather than inside the `if` -- and in
    // construction order, since each takes the one before it.
    // The reactor both framed surfaces share, and the connector over it.
    //
    // Declared BEFORE the tiers and therefore destroyed AFTER them, which is exactly
    // right and is why it is a local here rather than something each tier owns: a
    // tier's destructor closes its listener and its open connections by posting onto
    // this reactor, so the reactor has to still be running when that happens.
    //
    // It is NOT started yet. Every endpoint binds and adopts first, so that when the
    // thread does begin there is a listener behind every port a client might dial.
    Node::NodeIoLoop nodeIo;

    // The two framed COMPONENTS this node may serve besides its worker port, each
    // owned as one object. Both are off unless asked for: handing out other machines'
    // CPU time, and caching to this machine's disk, are decisions an operator makes
    // rather than things they get by starting a worker.
    //
    // Components rather than surfaces since #290: they share one `0xFC` listener,
    // opened below once both exist, and neither binds anything of its own. The order
    // here is therefore load-bearing in a new way -- the listener holds a reference to
    // each, so it is declared after both and destroyed before them.
    SteadyClock schedulerClock;
    SteadyClock cacheClock;

    // Wall-clock, and separate from the steady clocks beside it, because a lease
    // grant's expiry is checked on ANOTHER machine: a steady instant is meaningless
    // off the host that read it, while a system-clock instant is comparable anywhere
    // -- which is also why the check that reads it carries skew slack.
    SystemWallClock const schedulerWallClock;

    std::unique_ptr<Node::SchedulerTier> schedulerTier;
    if (cfg.serveScheduler)
    {
        auto started =
            Node::SchedulerTier::Start(cfg, membership.Oracle(), schedulerClock, schedulerWallClock, metrics, logger);
        if (!started.has_value())
        {
            // Fatal for the same reason the admin endpoint's is: an operator who asked
            // for this is relying on it, and a node that started without it looks
            // healthy to everything that would otherwise have noticed.
            logger.Logf(LogLevel::Error, "--serve-scheduler {}; refusing to start", started.error());
            return ExitUsage;
        }
        schedulerTier = std::move(*started);
    }

    // The node's own cache tier, in front of the shared one. It exists so a local
    // rebuild on a slow or bad network never reaches the wire: the shared cache holds
    // every object, but what is saved here is the round trip rather than the compile.
    //
    // Five collaborators in a reference chain, owned as one object rather than as
    // locals whose declaration ORDER is load-bearing and silently so.
    // Who may read this machine's build output is "this machine", full stop (#287)
    // -- not the member list beside it, which names peers that may spend this node's
    // CPU. The two questions were one list until a fleet peer could FETCH every
    // object this machine had ever compiled.
    //
    // The answer is ambient, so it arrives through a seam with a clock, and the set
    // is refreshed on an interval rather than per request: `GetAdaptersAddresses`
    // costs milliseconds on Windows, and a probe a stranger could provoke by asking
    // is a probe a stranger can bill this machine for. `CachedLocalityOracle`
    // carries both failure directions.
    auto const hostAddresses = MakeSystemHostAddresses();
    CachedLocalityOracle const cacheLocality { *hostAddresses, cacheClock };

    // ONE source, and every site that presents this worker's credential borrows it.
    // Declared here because the first of those sites is the cache tier immediately
    // below and the last is the heartbeat round far further down, and a local declared
    // between them would be an object two consumers reach and one of them outlives.
    //
    // It reads the RELOADER rather than `cfg`, which is the whole of #404: `cfg` is
    // the configuration this worker STARTED with, and a rotated `--requirepass` lives
    // in the snapshot the reloader publishes. A worker with no configuration file
    // passes null and gets `cfg`'s value forever, which is correct rather than a
    // fallback -- it has no second moment for anything to arrive at.
    Node::ConfiguredCredential const credential { cfg, reloader };

    auto cacheTierOrRefusal =
        Node::StartCacheTierOrExplain(nodeIo, cfg, credential, cacheLocality, cacheClock, metrics, logger);
    if (!cacheTierOrRefusal.has_value())
    {
        // No flag prefix here, unlike its neighbours: this tier can fail over two
        // different flags -- the directory and the port -- and only
        // `StartCacheTierOrExplain` knows which, so it names it.
        logger.Logf(LogLevel::Error, "{}; refusing to start", cacheTierOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: `StartCacheTierOrExplain` treats an emptied
    // `--listen-node`, and nowhere to keep objects, as reasons to
    // carry on without a tier rather than as failures. Both have been logged.
    auto const cacheTier = std::move(*cacheTierOrRefusal);

    // Computed HERE and advertised, rather than left for the scheduler to derive.
    // Both would use the same `OfferableSlots`, so the numbers would agree -- but
    // this worker has to enforce a limit before it has a scheduler to ask, and a
    // worker running to one number while the scheduler leases against another is
    // exactly the "fuller and slower than the scheduler believes" failure `--slots`
    // documents. One call, one answer, used for both.
    // The machine arrives through a seam rather than through `hardware_concurrency()`
    // and `QueryHostTotalMemoryBytes()` directly: `NodeCapacityOf` is what decides
    // which facts come from the operator and which from the hardware, and that rule
    // is only checkable if a test can present a two-core laptop and a 128-thread
    // server in one run. It also lives in `NodeConfig.cpp` rather than here, because
    // this file is in no test target.
    //
    // And it is computed BELOW the cache tier, which is the whole of #167: what a
    // compile cannot have is what the tier actually HOLDS, which is what the
    // configuration asked for only sometimes. `NodeCapacityOf`'s contract carries the
    // three ways those differ; this is the ordering that lets it be honoured.
    auto const host = MakeSystemHostFacts();
    auto const capacity =
        Node::NodeCapacityOf(cfg, *host, Node::CacheCapacityOf(cacheTier.get()), Node::IndexReserveBytesOf(cacheTier.get()));
    auto const slots = Distributed::OfferableSlots(capacity, cfg.slots);

    // Sized to the slot cap, which is what makes an admitted job always find a
    // thread: the cap admits at most `slots` at once, so one is free by
    // construction and nothing an operator can configure makes a job queue behind
    // another. Declared BEFORE the server, so the server -- which waits for its own
    // jobs in its destructor -- is torn down first and never outlived by the pool
    // it was handing work to. And before the 0xFC surface below, whose compiles run
    // on this same pool.
    ThreadPoolExecutor compilePool { slots };

    // **The accounting, with no accept loop around it.** `WorkerServer` was this object
    // plus a listener; #290 stage 3 retires the listener, and what is left is what the
    // merged surface was already spending. One slot cap, one byte budget, one bounded
    // drain -- so the figure this machine advertises describes the door that exists.
    Node::CompileCapacity compileCapacity {
        slots, Node::WorkerMaxRequestBytes, std::chrono::seconds { cfg.drainTimeoutSeconds }, logger
    };

    // The compile verbs on the node's own `0xFC` listener, which is #290's second
    // half. An ADDITIONAL door onto the worker above, never a second worker: it
    // spends `server.Capacity()` -- the same slot cap, the same byte budget, the
    // same bounded drain -- so what this machine advertises to the fleet describes
    // both doors rather than neither.
    //
    // The two executors are not interchangeable and the pair is the whole point. A
    // frame arrives on the reactor thread; the compile has to leave it, because a
    // process that blocks for seconds would stall every other connection that
    // reactor owns (#213); and the reply has to come BACK to it, because
    // `FrameEndpoint` writes what the responder returns to a reactor socket. The
    // second hop is the invisible one -- nothing in the type system or in a
    // functional test reports a reactor socket written from a pool thread -- which
    // is why `CompileResponder_test.cpp` asserts the thread identities.
    //
    // Declared AFTER the server whose capacity it spends and BEFORE the surface
    // that routes to it, so destruction runs surface, responder, server: the
    // listener stops admitting compiles before the drain starts counting them.
    Node::CompileResponder compileResponder { protocol, compileCapacity, membership.Oracle(), compilePool, nodeIo.Reactor(),
                                              metrics,  logger };

    // This node's one `0xFC` listener, opened once every component exists and holding a
    // reference to each (#290). Before the merge each tier bound its own port, which
    // made the listener the routing decision; now `MergedResponder` decides per frame
    // and the port is a property of the node rather than of any one component.
    //
    // Declared AFTER every component it routes to -- both tiers, the worker and its
    // responder -- and therefore destroyed BEFORE them, which is what keeps the router
    // from outliving what it routes to. And before consensus, because what a leader
    // advertises for the scheduler is what this BOUND.
    // What this node IS, for the operator verbs below. Its own clock, because uptime is
    // the one field that moves between requests and `Describe()` is asked per request.
    SteadyClock const statusClock;

    // **Observed, never inferred from a flag** -- with one deliberate exception, below.
    // A `--cache-dir` that would not open has already stopped startup, so a tier read off
    // the flag that asked for it would report a component that is not there; the pointer
    // is what actually started.
    //
    // `worker` is a literal `true` and that is not a shortcut: this binary compiles, that
    // is what it is for, and `compileResponder` above is unconditional. A predicate here
    // would be one nothing could make false.
    //
    // **That was the whole complaint in #1295, and the fix is NOT to make this bit
    // conditional.** The bit answers *does this node have a worker component*, and
    // `true` is the correct answer to it. The question it was being read for is *is that
    // worker serving anything yet*, which is a different question with three answers, and
    // it is now `runtimeState` below rather than a fourth reading of this bool.
    //
    // `consensus` is the exception, and it is the STRONGER answer rather than a weaker
    // one: the tier is constructed BELOW this point, so there is no pointer to read, and
    // `RunsConsensus` is the one predicate `StartConsensusOrExplain` itself asks (#1022,
    // #613). Reporting it cannot disagree with whether a tier gets built, which a second
    // spelling of the same question could.
    // Where the heartbeat thread publishes what the survey has concluded, and the ONLY
    // thing `Describe()` reads it through. The served map a few hundred lines below has
    // exactly one writer and no lock; these verbs are answered per request on a reactor
    // thread, so the map is not reachable from here and must not be made reachable.
    //
    // Seeded rather than defaulted, which is why `NodeRuntimeState` has no default
    // constructor: the honest first reading already exists at this point. The cheap half
    // of the survey has run -- `discoveredToolchains` is what it found -- and the
    // expensive walk has not, so this node is `Surveying` 0 of however many candidates
    // there are. A zero denominator here would be a reading nobody took.
    Node::NodeRuntimeState runtimeState { Node::ToolchainReading {
        .state = CompileCacheWire::ToolchainState::Surveying,
        .served = 0,
        .discovered = static_cast<std::uint32_t>(discoveredToolchains.entries.size()) } };

    Node::ConfiguredNodeStatus const nodeStatus {
        cfg,
        statusClock,
        startedAt,
        std::string { VersionString },
        cfg.nodeId,
        Node::NodeComponents { .cacheTier = cacheTier != nullptr,
                               .worker = true,
                               .scheduler = schedulerTier != nullptr,
                               .consensus = Node::RunsConsensus(cfg) },
        // `capacity` and `scheduler` are read LIVE; only `runtime` is published into.
        // Both are already in scope and already thread-safe, and a node running no
        // scheduler passes a null -- which is what makes an absent role mean *runs no
        // scheduler* rather than *is in an election*.
        Node::NodeRuntimeSources { .runtime = &runtimeState,
                                   .capacity = &compileCapacity,
                                   .scheduler = schedulerTier != nullptr ? &schedulerTier->Service() : nullptr },
    };

    // The operator verbs. Declared BEFORE the surface that routes to it and therefore
    // destroyed after, like every other responder here.
    //
    // `membership.Oracle()` bound once, by reference, the way every surface binds it: an
    // implementation re-asking for an oracle per request could never see `--fleet-open`
    // change, and a test that re-acquired it would pass under exactly that defect.
    Node::NodeStatusResponder nodeStatusResponder { nodeStatus, membership.Oracle(), metrics };

    auto nodeSurfaceOrRefusal = Node::StartNodeSurfaceOrExplain(
        nodeIo,
        cfg,
        // Designated, so the NAME travels with each pointer. Four bare
        // `IFrameResponder*` arguments are four a call site can silently transpose,
        // and a transposed pair routes every cache verb to the scheduler -- which
        // answers *served nowhere* for traffic this node is holding a tier for.
        Node::SurfaceComponents { .cache = cacheTier != nullptr ? &cacheTier->Responder() : nullptr,
                                  .scheduler = schedulerTier != nullptr ? &schedulerTier->Responder() : nullptr,
                                  .compile = &compileResponder,
                                  .node = &nodeStatusResponder },
        activated,
        metrics,
        logger,
        // Asked of the RUNNER rather than of a captured copy of the map, so a
        // re-survey that replaces the set is reflected in the very next line
        // (#238). `jobs` outlives the surface -- declared above it, destroyed
        // after -- which is what makes the reference safe to hold.
        [&jobs](std::string_view fingerprint) { return jobs.CompilerFor(fingerprint); });
    if (!nodeSurfaceOrRefusal.has_value())
    {
        // No flag prefix: this can fail over --listen-node or over --serve-scheduler,
        // and only `StartNodeSurfaceOrExplain` knows which, so it names the flag.
        logger.Logf(LogLevel::Error, "{}; refusing to start", nodeSurfaceOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null, and for exactly one reason: a node with none of the
    // three components serves no 0xFC port, which has been logged. A bind FAILURE is
    // not that case -- this surface's row states `Refuse`, so it arrives as the error
    // above rather than as a null. See `BindFailurePolicy` in NodeSurfaces.hpp.
    auto const nodeSurface = std::move(*nodeSurfaceOrRefusal);

    // Consensus, when the operator configured a cluster. It is what turns the
    // scheduler tier's standalone leadership into a real one: without it, every node
    // in a fleet believes it schedules, and two nodes handing out the same machine's
    // slots is the one thing the architecture says only one may do.
    //
    // Started AFTER the scheduler tier, because its observers push into it, and
    // declared after too, so it is destroyed first and cannot call into a tier that
    // has gone.
    auto consensusOrRefusal = Node::StartConsensusOrExplain(
        cfg, schedulerTier, nodeSurface != nullptr ? nodeSurface->BoundEndpoint() : std::string {}, membership, logger);
    if (!consensusOrRefusal.has_value())
    {
        // No flag prefix here, for the reason the cache tier's line below has none:
        // consensus fails over --node-id, --raft-peer, --listen-raft or
        // --cluster-dir, and only the tier knows which, so its message names the
        // flag. It used to prefix `--node-id `, which rendered the peer refusal as
        // "--node-id --node-id=n1 names no --raft-peer" -- and that message is now
        // `StartupPolicyRejection`'s own row, which names the flag by construction.
        logger.Logf(LogLevel::Error, "{}; refusing to start", consensusOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: no `--node-id` means this node leads alone.
    auto const consensusTier = std::move(*consensusOrRefusal);

    // Discovery, when the operator configured it. Declared AFTER consensus and so
    // destroyed before it, because its observer pushes into the tier above: a
    // discovery loop outliving the thing it hands peers to is a dangling reference
    // that only fires while a node is shutting down.
    auto discoveryOrRefusal = Node::StartDiscoveryOrExplain(cfg, consensusTier, logger);
    if (!discoveryOrRefusal.has_value())
    {
        // Fatal; why is `RowFor(NodeSurface::Discovery).bindFailureReason` (#352).
        // Not restated here -- this line and that row would be the two places the
        // ticket is about.
        logger.Logf(LogLevel::Error, "--discovery {}; refusing to start", discoveryOrRefusal.error());
        return ExitUsage;
    }
    // May legitimately be null: no `--discovery` means the cluster is the
    // `--raft-peer` list an operator typed, which is the ordinary deployment.
    auto const discoveryTier = std::move(*discoveryOrRefusal);

    // The admin endpoint, when the operator asked for one. Off by default and on
    // loopback for a bare port: a scrape surface reachable from the network is a
    // decision, not a default.
    //
    // The same `AdminHttpServer` the daemon runs, over the same renderer. A second
    // implementation for a process with no cache is what `MetricsSnapshot::storage`
    // being optional exists to avoid -- and it brings `/healthz`, which this worker
    // has never had: a supervisor could tell that the process was alive and not
    // that it was answering.
    //
    // Declared AFTER the cache tier, which is what makes the pointer its lambda
    // captures safe: locals are destroyed in reverse, so this endpoint stops
    // serving -- and joins its thread -- before the tier it reads is gone.
    // The admin surface: `/metrics`, `/healthz`, and the fleet dashboard when the
    // operator asked for one. Assembled by a function rather than here, and not
    // for tidiness -- `main.cpp` is in no test target, so the TLS, credential and
    // route-selection branches would otherwise have no coverage at all, and this
    // is the third time that reasoning has moved wiring out of `WorkerBody`.
    //
    // Declared AFTER the tiers it reads, which is what makes the pointers safe:
    // locals are destroyed in reverse, so the surface stops serving -- and joins
    // its thread -- before the scheduler, the cluster and the cache tier are gone.
    // Built by a function rather than spelled as a lambda here: a node with no cache
    // must report NO cache rather than an empty one, and that branch has to be
    // reachable from a test. Held in a local because the sampler and the scrape
    // surface both read it, and they must not disagree about the machine.
    auto snapshotProvider = Node::MakeNodeSnapshotProvider(
        Node::NodeScrapeSources { .host = host.get(),
                                  .busySlots = [&compileCapacity] { return compileCapacity.InFlight(); },
                                  .cache = cacheTier.get(),
                                  .slots = slots,
                                  .scratchRoot = jobs.ScratchRoot(),
                                  // Empty when this node runs no consensus, which is what makes the
                                  // scrape say NO cluster rather than a cluster of nobody. The tier is
                                  // declared above, so it outlives the provider: locals are destroyed in
                                  // reverse.
                                  .consensus = Node::ConsensusScrapeSource(consensusTier.get()) },
        startedAt);

    // Absent when this node runs no scheduler: there is then no registry to report,
    // so no fleet route is registered and `/fleet` is a plain 404.
    auto const fleetSources = schedulerTier
                                  ? std::optional { Distributed::FleetSources { .scheduler = &schedulerTier->Service(),
                                                                                // Legitimately null: a node with no
                                                                                // `--node-id` leads itself and has no
                                                                                // replicated state for anybody to read.
                                                                                .cluster = consensusTier.get(),
                                                                                .metrics = &metrics } }
                                  : std::nullopt;

    // EVERY node samples itself, whatever surfaces it serves. A pure worker runs no
    // dashboard and often no admin endpoint at all, and it is exactly the machine
    // doing the compiles -- so a sampler that existed only alongside a page would
    // leave the fleet's year with a hole where its busiest members should be.
    //
    // Declared AFTER the tiers it reads and BEFORE the surface that reads it, which
    // is what makes both sets of pointers safe: locals are destroyed in reverse.
    static SystemWallClock const wall;
    Node::FleetSampler sampler { fleetSources, metrics, snapshotProvider, wall, Node::HistoryPaths::For(cfg), logger };

    // Wired here because this is the one scope holding both: the scheduler tier is
    // built before the sampler that receives for it, so the sink cannot be a
    // constructor argument on either.
    if (schedulerTier != nullptr)
        schedulerTier->SetHistorySink(&sampler.Received());

    auto surfaceOrRefusal =
        Node::StartAdminSurfaceOrExplain(cfg, *host, metrics, std::move(snapshotProvider), fleetSources, &sampler, logger);

    // Fatal here and not in the daemon, which is a real difference between the two
    // binaries rather than a defect in either; the row says why
    // (`RowFor(NodeSurface::Admin).bindFailureReason`, #352).
    if (!surfaceOrRefusal.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", surfaceOrRefusal.error());
        return ExitUsage;
    }
    auto const adminSurface = std::move(*surfaceOrRefusal);

    // Both surfaces have bound and adopted, so the loop can start accepting. Doing
    // it here rather than at construction is the ordering `ConsensusTier::Launch`
    // already uses, and for the same reason: a client that dials the instant a port
    // is bound must not find a listener nobody is accepting on.
    //
    // A node with neither surface enabled adopts nothing and starts no thread.
    nodeIo.Start();

    // One registrar per toolchain, because REGISTER carries ONE fingerprint and
    // `--toolchain` is repeatable. Registering only the first -- which is what
    // this did -- meant a worker configured with g++ and clang++ served exactly
    // one of them, and which one depended on where two hex digests happened to
    // sort. The scheduler never heard about the other, so every job for it went
    // to a local compile with nothing anywhere reporting a reason.
    //
    // The scheduler keys a worker on (fingerprint, endpoint), so these are
    // separate entries with separate slot budgets -- which looks like advertising
    // N times this machine's capacity, and is not. Every entry heartbeats the
    // SAME machine-wide in-flight count, so once this worker is busy all of its
    // entries report themselves busy together and the scheduler stops picking any
    // of them. The pool behaves as one because the number it reports describes
    // the machine rather than the entry.
    //
    // `capacity` already carries the tier's record, so there is nothing to assemble
    // here. It used to be a patched copy -- the tier did not exist where `capacity`
    // was derived -- and since #167 it does, which is what makes the number this
    // worker enforces and the cache the leader renders one call rather than two.
    auto advertisedWire = Distributed::CapacityToWire(capacity);
    // Compiled in, never configurable. The point of the column this feeds is to tell
    // an operator which binary is actually running on each machine -- most often
    // part-way through a rolling upgrade -- and a version a node could be *told* to
    // report is one that can be wrong exactly when somebody is relying on it. It
    // travels inside the capacity record because that is REGISTER's one extensible
    // field; the message's own arity is exact and stays that way forever.
    advertisedWire.version = VersionString;
    // What a person calls this machine, and NOTHING decides from it (#1024). A node's
    // identity is minted into its state directory rather than typed, so an operator
    // reading an opaque id off the fleet page needs something that says which box that
    // is -- and the hostname is exactly the mutable, not-per-node value the identity
    // was deliberately not built on, which is why it may be a label and may be nothing
    // else. It rides in the capacity record beside `version` for the same reason:
    // REGISTER's own arity is exact and stays that way forever.
    advertisedWire.displayName = host->Facts().hostName;

    // A lambda, for the reason `compilersOf` is one: the heartbeat rebuilds this list
    // when the machine's toolchains change underneath the node, and two spellings of
    // what a registration carries is how a re-registered worker comes to advertise
    // something subtly different from the one it replaced.
    // One notice for every registrar this node builds, and it outlives them: the
    // heartbeat rebuilds the list when the machine's toolchains change, and a notice
    // per rebuild would say the same thing again on every change (#363).
    static Cc::CredentialNotice registrarNotice { [&logger](std::string_view text) {
        logger.Logf(LogLevel::Warn, "scheduler: {}", text);
    } };

    auto registrarsFor = [&](std::map<std::string, Node::ServedToolchain> const& served) {
        std::vector<Cc::WorkerRegistrar> built;
        built.reserve(served.size());
        for (auto const& [fingerprint, toolchain]: served)
        {
            // A copy per registrar, because the label is the one field of this record
            // that is NOT node-wide: a machine with two toolsets sends two
            // registrations describing one machine and two different compilers (#194).
            auto perToolchain = advertisedWire;
            perToolchain.toolchainLabel = toolchain.label;
            // The same list the worker protocol above answers in, and it governs the
            // OTHER direction too: the scheduler files it against this worker and the
            // grant relays it to the client, which compresses the preprocessed
            // translation unit against it. A literal `{ Identity }` here therefore sent
            // several megabytes per TU uncompressed as well (#265).
            built.emplace_back(registrarNotice, fingerprint, advertise, slots, Cc::AvailableCodecs(), perToolchain);
        }
        return built;
    };

    auto registrars = registrarsFor(toolchains);

    // Registrations this node has stopped serving and has not yet retired. Drained by
    // the next `AnnounceOnce`; see `HeartbeatRound::withdrawals`.
    std::vector<Cc::WorkerRegistrar> withdrawals;

    // What this node will TRY to serve, which since #365 is the only honest number
    // available at the ready line: the served map is empty until the heartbeat
    // thread's first round finishes walking the include trees, and printing its size
    // here would tell an operator "0 toolchain(s)" about a node that is starting
    // normally -- the one line they read to confirm the worker came up.
    //
    // Read from the DISCOVERED set, which this thread owns and no other touches.
    auto const startupToolchainCount = discoveredToolchains.entries.size();

    // It is also the denominator every `PublishToolchains` call below reports, on a
    // RE-survey as well as on the first one. `RefreshToolchains` answers `changed` and
    // `served` and no candidate count, so the alternative is to invent one; this is the
    // same number, from the same set, that the ready line above already reports, and it
    // is the only one this thread can state honestly.

    // One sampler for the whole loop, not one per heartbeat. CPU utilization is a
    // difference between two readings, so a sampler constructed per iteration would
    // have no earlier reading to difference against and would report nothing,
    // forever -- a scheduler that never learned this machine was busy, with nothing
    // anywhere saying so.
    //
    // Configured with the scratch path at construction rather than handed one per
    // call: which filesystem this worker writes to is a property of the worker, and
    // the "zero free bytes means the query failed, not that the disk is full" rule
    // belongs beside the query rather than at whoever remembers to apply it.
    auto const loadSampler = MakeHostLoadSampler(MakeSystemCounterSource(jobs.ScratchRoot()));

    // Registration and heartbeating are one loop because they are one concern: a
    // worker is registered exactly as long as it keeps saying so, and a scheduler
    // that has forgotten it answers the heartbeat by telling it to register again.
    // Splitting them would need the two halves to agree about which owns recovery.
    // A thread whose entire job is to block, which is the shape `IConnector`
    // names as correct for a dialler that has no reactor. The connector is a
    // `BlockingConnector` and is passed to `DialEndpointBlocking` by that type,
    // so the `SyncRun` inside it is sound by construction rather than by comment.
    BlockingConnector heartbeatConnector { DefaultAddressResolver(),
                                           BlockingConnectorOptions { .ioTimeout = HeartbeatIoTimeout } };

    /// Set when the fleet a scheduler registered this node into is not the one the
    /// operator asserted with `--cluster-id`. Fatal for the same reason
    /// `surveyFoundNothing` is: the node is running, but not as configured, and a
    /// supervisor restarting it will reach the same answer until somebody changes the
    /// flag or the scheduler this node is pointed at (#401).
    std::atomic<bool> fleetAssertionFailed { false };

    Node::HeartbeatRound const round { .cfg = cfg,
                                       .registrars = registrars,
                                       .withdrawals = withdrawals,
                                       .capacity = compileCapacity,
                                       .loadSampler = *loadSampler,
                                       .cacheTier = cacheTier.get(),
                                       .metrics = metrics,
                                       .sampler = sampler,
                                       .credential = credential,
                                       .lease = leaseState,
                                       .fleetMismatch = fleetAssertionFailed,
                                       .logger = logger };

    // Counts heartbeats, so the slow sweep below has a cadence of its own. A local of
    // the thread's lambda rather than a member of anything: only this thread reads or
    // writes it.
    std::uint64_t beat = 0;

    // The configuration snapshot this thread last surveyed against, so a reload is
    // noticed by COMPARISON rather than by a flag somebody else sets.
    //
    // Seeded BEFORE the initial survey rather than left null, which is what makes the
    // first beat comparable to every other one: the survey is expensive and a reload
    // can land while it runs, so a baseline taken afterwards would either miss that
    // reload or -- taken as "null means new" -- treat every ordinary start as one.
    // Null only when this worker has no configuration file at all.
    ConfigReloaderOf<NodeConfig>::Snapshot actedOn = reloader != nullptr ? reloader->Current() : nullptr;

    // Where this node believes the scheduler's leader is. Declared out here so it
    // survives across rounds -- remembering the leader is the whole reason a
    // steady-state fleet does not spend a redirect on every heartbeat -- and, like
    // `beat`, touched by this one thread only, which is what makes it safe without
    // a lock of its own. It outlives the `jthread` below, which joins in its
    // destructor before any of this goes.
    Node::SchedulerLink link { cfg.scheduler };

    // Set by the heartbeat thread when the initial survey answers with nothing, read
    // by this one at the return below.
    //
    // "Nothing to serve" was a startup refusal before #365 and stays one: left
    // running, a worker with nothing to serve is the worst shape this system has --
    // it registers nothing, so the scheduler never hears of it; the heartbeat reports
    // "0 of 0 toolchain(s) registered" and calls that a complete success; the ready
    // line says the node is up. A supervisor sees a healthy unit, an operator sees a
    // green fleet, and every build compiles locally with no error at either end.
    //
    // What #365 changes is only WHEN it is said, not whether. The common
    // misconfiguration -- no compiler at all, a malformed `--toolchain` -- is still
    // refused promptly, above, because discovery stayed on the startup path. What
    // arrives late is the narrow case where compilers were found, spawned, and then
    // every one of them failed to yield a usable identity. `ResolveToolchains` has
    // already logged which, and the exit code is what a supervisor reads.
    std::atomic<bool> surveyFoundNothing { false };

    std::jthread const heartbeat { [&](std::stop_token const& stop) {
        // The initial survey, and it runs HERE rather than at startup because it is
        // the expensive half: a full walk of every include tree, measured over 300 s
        // on a cold Windows runner (#354). Off the startup path, the node has already
        // bound its port, brought up its cache tier and its admin surface, and said
        // it is ready -- so a machine that takes minutes to identify its toolchains
        // spends those minutes SERVING rather than silent.
        //
        // On this thread and not a new one, deliberately: this thread already owns
        // `toolchains`, already calls `ReplaceToolchains` and `registrarsFor` on
        // every re-survey (#238), and already runs before the first announcement. A
        // dedicated survey thread would be a second writer to all three and would
        // need a lock that the re-survey path has never needed.
        // The stop token is passed, and that is not a courtesy. This runs on the
        // heartbeat thread, whose `jthread` destructor joins before `WorkerBody`
        // returns, and `InstallNodeStopHandlers` runs a few lines below -- so a
        // SIGTERM arriving here is CAUGHT rather than fatal, and an unobservable walk
        // would make the process wait out the whole survey before it could finish
        // stopping. Minutes, on the cold machine this ticket is about, which a
        // supervisor answers with SIGKILL and no diagnostic.
        //
        // Before #365 this work ran on the main thread, before any handler existed,
        // so the same signal simply killed the process. Moving it here is what made
        // cancellation something that has to be spelled.
        auto surveyed =
            Node::FingerprintToolchains(discoveredToolchains, *runner, *toolchainHost, toolchainClock, logger, stop);
        switch (surveyed.outcome)
        {
            case Node::SurveyOutcome::Served:
                toolchains = std::move(surveyed.served);
                // The same two calls, in the same order, as the re-survey below: the
                // compile port first and the registration second, so this worker
                // never announces a fingerprint it is not yet ready to serve. One way
                // into the serving state rather than two.
                jobs.ReplaceToolchains(compilersOf(toolchains));
                Node::AdoptRegistrars(registrarsFor(toolchains), toolchains, registrars, withdrawals);
                // AFTER the two calls above, never before: this says the worker is
                // serving, and it must not say so while the compile port still holds the
                // previous answer.
                PublishToolchains(runtimeState, toolchains.size(), startupToolchainCount);
                break;
            case Node::SurveyOutcome::NothingToServe:
                // Published even though the process is on its way out. The stop is not
                // instant -- the watcher below has to notice it -- and this is exactly
                // the window in which an operator asking *what is wrong with that node*
                // gets a real answer instead of `Surveying` forever.
                PublishToolchains(runtimeState, 0, startupToolchainCount);
                surveyFoundNothing = true;
                DaemonControls::Instance().RequestStop();
                return;
            case Node::SurveyOutcome::NoneCouldBeAsked:
                // **Not fatal, and `surveyFoundNothing` stays false** (#1060). Every
                // candidate that left did so because it could not be asked -- a
                // compiler mid-upgrade, a fork that failed under momentary pressure --
                // and this node is not misconfigured, so exiting `2` would tell a
                // supervisor a configuration error it does not have and take a
                // recoverable machine out of the fleet for good.
                //
                // Falls THROUGH to the heartbeat loop rather than returning, which is
                // the whole remedy: `registrars` stays empty so nothing is announced,
                // the compile port serves nothing, and the periodic sweep below
                // surveys the machine again. That loop already treats a machine that
                // loses its last compiler this way, for the reason written there --
                // a routine upgrade must not be able to remove a machine from the
                // fleet permanently. Only the FIRST survey lacked it, and only
                // because it could not tell the two empty surveys apart.
                //
                // `FingerprintToolchains` has already said so at Warn, naming the
                // count, so nothing is logged twice here.
                //
                // It DOES publish, and this arm is the one that most needed a verb:
                // the node stays up serving nothing, which read from outside as a
                // healthy worker with an idle fleet. `NothingToServe` on the wire
                // rather than a fourth state naming this cause -- the re-survey below
                // cannot tell the two empty surveys apart, so a state only the first
                // survey could fill would be guessed on every beat after it. See
                // `CompileCacheWire::ToolchainState`.
                PublishToolchains(runtimeState, 0, startupToolchainCount);
                break;
            case Node::SurveyOutcome::Cancelled:
                // Already stopping, and `surveyFoundNothing` stays false: this node
                // was not misconfigured, it was interrupted, and exiting non-zero
                // would tell a supervisor to restart something that was asked to
                // stop.
                return;
        }

        while (!stop.stop_requested())
        {
            // Asked BEFORE the announcement, so a machine whose compiler was patched
            // since the last round registers under the identity it can actually
            // honour rather than announcing the old one once more (#238).
            //
            // On this thread and nowhere else, which is what makes the two mutations
            // below safe without a lock of their own: `registrars` is read only by
            // `AnnounceOnce`, three lines down and on this same thread, and
            // `ReplaceToolchains` takes the runner's own lock against the compile
            // threads. The check itself spawns nothing -- it is a stat per compiler
            // and one per include root -- so it costs a heartbeat almost nothing and
            // pays for the survey only when something moved.
            // Every beat asks the witnesses; one beat in `SweepEveryBeats` surveys the
            // machine regardless. The sweep is the only way back from serving LESS
            // than this machine has: a recheck driven by witnesses can only notice
            // what it is already watching, so a toolchain dropped by a transient
            // probe failure -- or removed and later reinstalled -- would otherwise
            // never be looked at again, and a node left serving nothing could not
            // recover at all without a restart.
            ++beat;

            // The configuration this beat acts on, which is the RELOADED one when a
            // file exists. Read once and held for the whole beat: `Current()` can be
            // swapped underneath by a reload arriving mid-beat, and deciding the depth
            // against one configuration and then surveying against another is how a
            // re-derivation gets skipped for the very change that asked for it.
            auto const snapshot = reloader != nullptr ? reloader->Current() : nullptr;
            auto const& liveCfg = snapshot ? *snapshot : cfg;

            // Three ways to take the expensive path, and the third is #403's. A
            // witness moved (the cheap check inside `RefreshToolchains`); the periodic
            // sweep came round, which is the only way back from serving LESS than this
            // machine has; or an operator edited the file, which moves no witness at
            // all and would otherwise wait for that sweep.
            //
            // A pure function rather than the expression that used to stand here, for
            // the reason `RecheckDepthFor` and `SurveyVoiceFor` below already give:
            // this file is in no test target, so the JOIN between two tested decisions
            // was the one step verified only by reading (#587). Everything the
            // expression encoded -- why it is computed here rather than signalled from
            // the reload, and why a null `actedOn` is "no configuration file" rather
            // than "first beat" -- travelled with it into that function's header.
            auto const reloaded = Node::ClaimsReloadedBetween(actedOn, snapshot);
            actedOn = snapshot;

            // Compared SEPARATELY from `reloaded` above, and that is the point rather
            // than duplication. `AdvertisedClaimsDiffer` asks whether what this worker
            // TELLS the fleet has changed, and extending the allowlist changes nothing
            // it advertises -- so gating this on that answer would be a reload an
            // operator watched do nothing, which is the failure #403's own row warns
            // about one setting over.
            //
            // Whether anything is said, and what, is `AllowlistAnnouncement`'s rule
            // rather than this loop's -- an emptied set is said out loud too, because an
            // operator who removed every entry needs to see that it took effect as much
            // as one who added the first.
            AdoptAllowlist(jobs, logger, appliedExtraArgs, liveCfg.extraAllowedArgs);
            auto const depth = Node::RecheckDepthFor(reloaded, beat, SweepEveryBeats);

            // How loudly that survey narrates itself. The timer sweep whispers; a
            // reload and a moved witness do not (#993). Decided by a pure function
            // rather than here, for `RecheckDepthFor`'s own reason: this file is in
            // no test target.
            auto const voice = Node::SurveyVoiceFor(reloaded, depth);

            if (auto refreshed = Node::RefreshToolchains(toolchains,
                                                         liveCfg,
                                                         discoveryFor(liveCfg),
                                                         *runner,
                                                         *toolchainHost,
                                                         toolchainClock,
                                                         logger,
                                                         depth,
                                                         voice);
                refreshed.changed)
            {
                toolchains = std::move(refreshed.served);

                // The compile port first, the registration second. Between the two
                // this worker refuses a job naming the dropped fingerprint rather
                // than serving it with the new compiler, which is the wrong-object
                // path this exists to close; the other order would leave that window
                // open for a whole heartbeat.
                jobs.ReplaceToolchains(compilersOf(toolchains));
                Node::AdoptRegistrars(registrarsFor(toolchains), toolchains, registrars, withdrawals);
                // Same order and the same reason as the first survey's `Served` arm.
                // Inside `changed` deliberately: an unchanged sweep concluded nothing
                // new, and republishing on every beat would make a reading that has not
                // moved look like one that was re-taken.
                PublishToolchains(runtimeState, toolchains.size(), startupToolchainCount);

                // A worker that ends up serving nothing keeps running and keeps
                // saying nothing, rather than exiting: the compiler may come back
                // with the next package, and a routine upgrade must not be able to
                // remove a machine from the fleet permanently.
                //
                // Its entries used to reach the registry's timeout on their own, and
                // since #573 the line above RETIRES them instead. That does not weaken
                // the rule this paragraph is about: a withdrawal is no more permanent
                // than an expiry, because the next survey that finds a compiler
                // registers again. What it removes is the 90 seconds in between, during
                // which the scheduler would go on picking a worker that has already
                // stopped serving the fingerprint and would refuse every job it sent.
                if (toolchains.empty())
                    logger.Logf(LogLevel::Warn,
                                "this machine now has no usable toolchain; serving nothing until one returns");
            }

            PublishRegistration(runtimeState, statusClock, registrars, AnnounceRound(round, link, heartbeatConnector));

            // Slept in slices so a stop request is observed promptly: a worker that
            // took a full heartbeat interval to exit would hold its port that long
            // against a restart.
            for (int slice = 0; slice < HeartbeatSlices && !stop.stop_requested(); ++slice)
                std::this_thread::sleep_for(HeartbeatInterval / HeartbeatSlices);
        }
    } };

    // Installed only once the listener is up and the heartbeat is running, so a
    // stop arriving during startup cannot close a listener that does not exist yet.
    InstallNodeStopHandlers();

    // The watcher exists because the two halves of a stop cannot be the same
    // thread: the signal handler may only set a flag, and the accept loop is parked
    // inside Accept() and cannot look at one. Closing the listener is what unparks
    // it -- on POSIX via the poll timeout the loop already treats as "not a
    // failure", which is the mechanism WorkerServer::Run documents.
    //
    // Run() returning does NOT mean the worker is idle: since #213 a compile is
    // detached onto `compilePool`, so jobs admitted before the listener closed are
    // still running. ~WorkerServer is what waits for them, and it runs before
    // ~ThreadPoolExecutor because `server` is declared after the pool.
    //
    // Nor does it mean nothing more can arrive: since #290 a compile also reaches the
    // merged 0xFC surface, which this loop knows nothing about. The declaration order
    // is what makes that safe -- the surface is declared after `server`, so it stops
    // accepting first -- and the one drain covers both doors regardless, because both
    // spend the same `CompileCapacity`.
    // The listening endpoint is described by where it CAME FROM, not by the
    // config. When a socket was adopted, `--bind` and `--port` were never used,
    // and printing them names an address this process is not listening on -- which
    // in the one line an operator reads to confirm a worker came up is worse than
    // printing nothing. What matters to a client is the advertised endpoint, and
    // that is reported either way.
    // The endpoint this node actually LISTENS on, which since #290 stage 3 is the one
    // 0xFC surface -- there is no second port to disambiguate and no config value that
    // describes a socket this process did not open.
    auto const listeningOn = Node::AdvertisedEndpoint(cfg);
    // The admission policy is part of this line, and that is #235's second half: a
    // worker given no policy starts, binds the wildcard, registers, is leased out
    // and refuses every dispatched compile -- and until this said so, the one line
    // an operator reads to confirm the worker came up reported nothing but health.
    // The scheduler tier prints the same phrase from the same function, so a node
    // running both says one thing rather than two.
    logger.Logf(LogLevel::Info,
                // The marker text is a row of `ReadinessMarkerTable`, never a literal here.
                // Four fixtures in two languages wait on these bytes and this build recompiles
                // none of them, so a reword breaks them by TIMEOUT rather than by a failed
                // build (#654). Referencing the row is what makes a rename a compile error.
                "{} on {}, advertising {}, {} slot(s) as a {} node, identifying {} toolchain(s), {}",
                ReadinessMarkerText(ReadinessMarker::CompileNode),
                listeningOn,
                advertise,
                slots,
                Distributed::TraitsFor(cfg.nodeClass).name,
                // The count this node is BRINGING UP, not the count it is serving:
                // the survey runs on the heartbeat thread and has almost certainly
                // not finished when this prints (#365). Reading `toolchains` here
                // would race that thread as well as understate it -- and a ready
                // line is a statement about starting anyway.
                startupToolchainCount,
                Node::AdmissionSummary(cfg));

    // **Main waits here now, and there is no accept loop to interrupt.** This was
    // `SyncRun(server.Run())` -- the dedicated listener's accept loop, which blocked
    // this thread until a stop closed it, and which needed a watcher thread to notice
    // the stop at all. The merged surface's loops run on `nodeIo`'s own thread, so what
    // is left for main to do is wait for the stop and then drain.
    //
    // That deletes the watcher rather than rehoming it: it existed only because a
    // parked `accept()` cannot be woken by a flag.
    while (!DaemonControls::Instance().StopRequested())
    {
        // Taken here rather than in the handler, for the reason the handler's own note
        // gives. `TakeReloadRequest` clears the flag atomically, so a burst of SIGHUPs
        // costs one re-read rather than one per signal.
        //
        // Nothing is handed to the heartbeat thread: it compares snapshots itself, so
        // this call publishes a new configuration and says so, and the next beat picks
        // it up on its own.
        if (DaemonControls::Instance().TakeReloadRequest())
            Node::ApplyReloadRequest(reloader, membership, logger);
        std::this_thread::sleep_for(StopPollInterval);
    }
    logger.Logf(LogLevel::Info, "stop requested; no longer accepting compiles");
    compileCapacity.BeginShutdown();

    // **Both doors closed and every compile drained HERE, while the node's reactor is
    // still turning.** Not tidiness and not a duplicate of the destructor: a compile
    // admitted through the merged `0xFC` surface finishes on `compilePool` and then
    // hops back onto `nodeIo`'s reactor to hand back its reply. That reactor stops when
    // the last ADOPTED loop ends, and a connection task parked off-reactor is not one
    // of them -- so tearing the surface down first can stop the reactor with a compile
    // still out, losing the hop home, the slot and the byte reservation, and leaving
    // this worker to wait out its whole drain timeout and `_Exit` reporting compiles
    // still running that had already finished.
    //
    // Destruction order cannot express this: the surface points at the responder and
    // the responder at this worker's capacity, so those three must be destroyed
    // surface-first, which is the opposite of what the drain needs. Separating the stop
    // from the destruction is what lets both be right, and it is why `StopAndWait`
    // exists as something callable rather than only as a destructor body.
    compileCapacity.Drain();

    // Unwired BEFORE the sampler goes, and that ordering is the whole reason this
    // line exists: locals are destroyed in reverse declaration order, so the
    // scheduler tier -- declared before the sampler because the sampler reads its
    // registry -- outlives the store it routes to. A heartbeat arriving in that
    // window would file a machine's buckets through a pointer to a destroyed one.
    if (schedulerTier != nullptr)
        schedulerTier->SetHistorySink(nullptr);

    // Unblocks the admin accept loop so its jthread can join -- without this the
    // destructor's implicit join waits on a thread parked in accept(), and a worker
    // that stops cleanly would hang instead. The same reason the daemon does it.
    // `adminEndpoint`'s destructor stops the server and joins its thread; there is
    // deliberately nothing to remember at this return path or any other.

    // No deregistration is sent, and that is a decision rather than a gap. There is
    // no such verb: the scheduler learns a worker is gone by its heartbeat lapsing,
    // which is the ONE mechanism that also covers the cases a polite goodbye cannot
    // -- a killed process, a severed network, a crashed host. Adding a second path
    // would mean the fleet had two ways to believe a worker is alive and only one of
    // them exercised in the failure that matters. A client that leases this worker
    // in the gap finds it unreachable and compiles locally, which is the same
    // fallback every other refusal takes.
    logger.Logf(LogLevel::Info, "compile node stopped");
    // A node that came up, served, and then found it had nothing to compile with
    // exits as the refusal it would have been before #365 -- late, but with the same
    // code and the same diagnostic. A supervisor that restarts on failure must not
    // read this as a clean stop.
    return surveyFoundNothing || fleetAssertionFailed ? ExitUsage : ExitOk;
}

} // namespace

int main(int argc, char** argv)
{
    std::span<char const* const> const argvSpan { const_cast<char const* const*>(argv), static_cast<std::size_t>(argc) };

    // Parsed TWICE, into two results, and which one a decision reads is the whole
    // of this arrangement.
    //
    // `cliOnly` holds nothing but the command line. It answers the two questions a
    // configuration file must not be able to answer: WHICH file to read, and what
    // gets baked into a service registration -- `--install-service` registers the
    // command line as typed, so a setting the file supplied must not be copied into
    // launch arguments that then outrank the file forever.
    //
    // `cfg` is the file applied first and the command line applied over it, through
    // the SAME appliers in that order. "The command line wins" is therefore the
    // order two loops run in rather than a per-field merge with a per-field
    // explicit bit -- which is the shape the daemon has, and which has shipped a
    // flag that parsed but never merged four times.
    NodeConfig cliOnly;
    auto const flow = ParseOptionsInto(NodeOptions(), argvSpan.subspan(1), cliOnly);
    if (!flow.has_value())
    {
        // The FIELD as well as the reason. Without it an unrecognised argument reads
        // as `unrecognised argument` and names nothing at all -- the daemon and the
        // test client have always printed both, and this is the binary whose flags
        // an operator is most likely to be typing by hand.
        std::cerr << "fastcache-compile-node: " << flow.error().field << ": " << flow.error().context << '\n';

        // Said only where it explains something. A non-ASCII argument does not reach
        // a Windows process as UTF-8 unless its active code page is UTF-8, which
        // every executable here asks for by manifest -- so this line appears only on
        // a host too old to honour that, where it is the whole answer and no other
        // surface would ever give it (issue #155).
        //
        // The optional is tested rather than `NarrowTextIsUtf8()`, which implies but
        // does not state that there is a code page to print: an inference a reader
        // has to make is one clang-tidy makes differently.
        if (auto const codePage = ActiveCodePage(); codePage.has_value() && *codePage != Utf8CodePage)
            std::cerr << "fastcache-compile-node: this host's active code page is " << *codePage
                      << ", not UTF-8, so a non-ASCII argument does not reach this process as the bytes you typed\n";
        return ExitUsage;
    }

    if (cliOnly.help)
    {
        // The color decision is made here rather than inside the help renderer so
        // that module stays free of ambient probes -- and on Windows the call also
        // enables virtual-terminal processing, so it must precede any output.
        std::cout << HelpText(StdoutSupportsColor() ? UsageColor::Colored : UsageColor::Plain);
        return 0;
    }
    if (cliOnly.version)
    {
        std::cout << "fastcache-compile-node " << FASTCACHE_NODE_VERSION << '\n';
        return 0;
    }

    // Both of the above answer without reading anything, deliberately: a file this
    // build cannot parse must not be able to stop `--help` from explaining the flag
    // that would name a different one.

    // A leftover from when this worker was configured by a bag of arguments in an
    // EnvironmentFile. The unit no longer reads it, so a value left behind would
    // silently stop taking effect -- an operator's settings disappearing at an
    // upgrade with nothing anywhere saying why. Said once, at every start, because
    // the remedy is to delete the file and there is nothing else to report.
    if (ReadEnvironmentVariable("FASTCACHE_NODE_ARGS").has_value())
        std::cerr << "fastcache-compile-node: FASTCACHE_NODE_ARGS is set and is no longer read; this worker is "
                     "configured by --config=<file> (see /etc/fastcached/fastcache-compile-node.yaml). Delete "
                     "the leftover /etc/fastcached/compile-node.env and put those settings in the YAML file.\n";

    // The file the operator named, or else whichever platform default is actually
    // there. `EffectiveConfigPath` owns that rule -- a named path is strict and a
    // discovered one is skipped when it is absent, unreadable or untrusted -- and
    // it is the same rule and the same code the daemon's lookup uses.
    auto const lookup = EffectiveConfigPath(cliOnly.configPath, SystemConfigPathProbe {}, NodeApplicationName);

    // A file that is there and readable and was passed over anyway has to say so.
    // Silence would leave an operator editing a file this worker has quietly
    // decided not to obey, over a permission problem only they can fix.
    for (auto const& [rejectedPath, reason]: lookup.rejected)
        std::cerr << "fastcache-compile-node: " << rejectedPath.string() << ": " << reason << '\n';

    // A file this run cannot use is fatal -- with ONE exception, and it is the same
    // one the daemon carves out and for the same reason. `--uninstall-service`
    // names a registration to remove and reads nothing out of the file; refusing to
    // run it because the file at the default location has a typo blocks the very
    // recovery an operator reached for, and the registration being removed is
    // frequently the thing that put the bad file there.
    //
    // Narrower than the daemon's carve-out, deliberately. `--install-service` is
    // judged against the merged configuration, `--print-surfaces` prints it, and a
    // cluster verb dials an endpoint that may come from it -- so for those three, a
    // file that did not load would produce a confident answer about a configuration
    // this process never assembled.
    //
    // A path the operator NAMED is fatal even then: they are owed the news that the
    // file they typed did not arrive.
    auto const fileIsAdvisory = cliOnly.uninstallService && cliOnly.configPath.empty();

    NodeConfig cfg;
    bool fileApplied = false;
    if (!lookup.path.empty())
    {
        auto const loaded =
            ReadYamlSettings(lookup.path).and_then([&cfg, &lookup, argvSpan](std::vector<YamlSetting> const& settings) {
                return ApplyNodeConfiguration(settings, lookup.path, argvSpan.subspan(1), cfg);
            });
        if (loaded.has_value())
            fileApplied = true;
        else if (!fileIsAdvisory)
        {
            std::cerr << "fastcache-compile-node: " << loaded.error().ToString() << '\n';
            return ExitUsage;
        }
        else
            std::cerr << "fastcache-compile-node: ignoring " << lookup.path.string() << ": " << loaded.error().ToString()
                      << '\n';
    }

    if (!fileApplied)
        // Assigned rather than left as it is: a file that failed halfway leaves
        // `cfg` holding part of a document this run has decided not to obey, and
        // "some of the settings, up to the bad line" is a configuration nobody
        // wrote. Declining a file means the command line stands alone -- which is
        // also the ordinary no-file case, where `cliOnly` is already the answer.
        cfg = cliOnly;

    // The admin verbs below answer an operator at a terminal, so they report to one
    // -- even under `--daemon`, which the registered command line carries and which
    // is therefore exactly what somebody copies out of `sc qc` to try by hand. Their
    // refusals going to the event log while the terminal showed only an exit code
    // would be the same defect this file is fixing, pointed the other way.
    // Built by the factory, never here: the third argument used to be absent at this
    // very line, so the node could not emit a time and no flag existed to ask for one
    // (#485). Omitting it no longer compiles -- `ConsoleLogger` takes `LogTimestamps`
    // with no default -- and the factory is what carries the CONFIGURATION to it,
    // which is the half a signature cannot check.
    auto const consoleLogger = MakeNodeConsoleLogger(std::cerr, cfg);

    // **Moved BELOW the logger and still above the startup table** (#582). It is one of
    // the terminal-facing verbs the comment above is about, and its refusal has to
    // reach the same terminal, rendered the same way as the one a start prints -- an
    // operator who meets this sentence here and again at boot should be reading one
    // message, not matching two. The ordering that matters is unchanged: this is still
    // ahead of `StartupPolicyRejection`, so the worksheet is printed for a broken
    // configuration exactly as before, and only the exit code moved.
    if (cfg.printSurfaces)
    {
        // Before the startup rules below, deliberately. An operator reaches for this
        // BECAUSE a port is wrong, and refusing to show the map until the
        // configuration is already valid would withhold it exactly when it is wanted.
        // It opens nothing and changes nothing, so there is no state to protect.
        //
        // **The ordering is right; the exit code was the defect** (#582). This printed
        // the map and returned 0 for a configuration the node then refuses to run --
        // measured: the documented scheduler line without `--cluster-key-file` exits 2
        // on its own and exited 0 through this flag. Since the flag prints the RESOLVED
        // configuration, an operator reaches for it before writing a unit file, and it
        // answered "fine" to a command line with no chance of starting.
        //
        // Printing the map for a broken configuration is the feature; reporting SUCCESS
        // for it is not. `ReportSurfaces` renders and judges in one call so this site
        // decides nothing: the verdict is `StartupPolicyRejection`'s own sentence, and
        // the exit code follows from whether there is one. See that function for why it
        // is a function rather than four lines here -- five verbs answer and exit ahead
        // of the startup table, and they do not all want the same answer.
        auto const report = ReportSurfaces(cfg);
        std::cout << report.text;
        if (!report.refusal.has_value())
            return ExitOk;
        consoleLogger->Logf(LogLevel::Error, "{}", *report.refusal);
        return ExitUsage;
    }

    // Seeding, ahead of the startup table for `--print-surfaces`' reason: it is an
    // INSTALLER step, run before this machine has a working configuration at all, so
    // refusing it until the configuration is already valid would make it unusable at
    // the only moment it is wanted.
    //
    // The WORKER seeds its own file. `fastcached --seed-config` derives its
    // destination from `DaemonApplicationName`, so it can only ever write the daemon's
    // — which is why the MSI shipped no worker configuration and the .pkg shipped none
    // either (#397). Deriving it here from this binary's own application name is what
    // makes the seeded path and the path the startup lookup walks one answer rather
    // than two that agree until somebody edits one.
    if (!cfg.seedConfigTemplate.empty())
    {
        auto const seeded =
            SystemConfigPath(SystemConfigPathProbe {}, NodeApplicationName).and_then([&](auto const& destination) {
                return SeedConfigFile(cfg.seedConfigTemplate, destination, DirectoryPolicy::AdministratorsOnly);
            });
        if (!seeded.has_value())
        {
            consoleLogger->Logf(LogLevel::Error, "{}", seeded.error().ToString());
            return ExitUsage;
        }
        consoleLogger->Logf(LogLevel::Info, "{}", SeedOutcomeSentence(*seeded, cfg.seedConfigTemplate));
        return ExitOk;
    }

    // Service registration, before anything that costs time. A misconfiguration is
    // decided in microseconds while a toolchain fingerprint takes seconds, which is
    // the same cheap-and-fallible-first ordering the socket-activation check follows.
    //
    // The command line is registered as typed. Every other flag alongside
    // --install-service is baked in and reused at every start, so a registration
    // that cannot work must fail here, where an operator is watching, rather than
    // at every boot where nobody is. That is why the gate is
    // `NodeInstallRejection` and not `NodeServiceRejection`: an install has to
    // satisfy the STARTUP rules as well, since this returns before they are ever
    // reached and every one of them is decided by the command line being baked in.
    if (cfg.installService || cfg.uninstallService)
    {
        // Only an install has to be viable; an uninstall merely names a
        // registration to remove, and refusing to remove one because it was
        // misconfigured is how a bad registration becomes permanent.
        //
        // Judged on the MERGED configuration, and registered from the command line
        // alone -- which is not a contradiction. What the service will run with is
        // the file plus these arguments, so judging the command line alone would
        // refuse the documented setup, where `--scheduler` and the toolchains come
        // out of the packaged file. What is baked in is still only what was typed,
        // plus the `--config` path that supplies the rest.
        if (cfg.installService)
            if (auto const rejection = NodeInstallRejection(cfg))
            {
                consoleLogger->Logf(LogLevel::Error, "{}", *rejection);
                return ExitUsage;
            }

        // The identity the registration will bake in, resolved here because a
        // registration replays its command line forever: one that omitted it would let
        // a re-image answer to an identity the cluster never admitted, with both
        // machines up throughout. AFTER `NodeInstallRejection`, so a command line this
        // install is about to refuse leaves no state directory behind, exactly as the
        // start path resolves after its own table.
        //
        // An uninstall reaches neither -- `NodeIdentityNeed` declines it -- because
        // removing a registration is the recovery an operator reaches for when the
        // configuration is already wrong.
        SystemRandomSource identityRandom;
        if (auto const adopted = AdoptNodeIdentity(cfg, cliOnly, identityRandom, *consoleLogger); !adopted.has_value())
        {
            consoleLogger->Logf(LogLevel::Error, "{}; refusing to install", adopted.error());
            return ExitUsage;
        }

        // `cliOnly`, never `cfg`: a registration replays its arguments at every
        // start, so baking in what the FILE said would freeze one reading of that
        // file into launch arguments that then outrank the file itself -- the
        // operator edits it, restarts the service, and nothing changes. What the
        // registration does carry is the `--config` path, so the service reads the
        // current file at every start rather than a snapshot of it.
        auto const spec = MakeNodeServiceSpec(CurrentExecutablePath(), cliOnly);
        auto const result =
            cfg.installService ? InstallService(spec, cfg.serviceScope) : UninstallService(spec, cfg.serviceScope);
        if (result.exitCode == 0)
            std::cout << "fastcache-compile-node: " << result.message << '\n';
        else
            std::cerr << "fastcache-compile-node: " << result.message << '\n';
        return result.exitCode;
    }

    // Converting the store acts on the files and exits. After the service block
    // for the same reason that one is early -- it costs microseconds to decide --
    // and before everything below it, because a node whose store is of the wrong
    // vintage cannot start at all: making the operator satisfy `--scheduler` or a
    // toolchain probe first would be demanding they fix a running configuration
    // before being allowed to fix the store that stops it running.
    if (cfg.migrateCache)
    {
        auto const outcome = MigrateDiskTier(cfg);
        if (!outcome.has_value())
        {
            consoleLogger->Logf(LogLevel::Error, "{}", outcome.error());
            return ExitUsage;
        }
        std::cout << "fastcache-compile-node: " << *outcome << '\n';
        return 0;
    }

    // A question asked OF a running cluster, rather than a worker starting up.
    // After the service block, because an installation is about this machine and
    // this is about somebody else's; before the `--scheduler` and `--toolchain`
    // checks, because a cluster command needs the first and not the second.
    if (cfg.cluster.action != ClusterAction::None)
    {
        // No reloader exists at this point and none ever will on this path: a
        // cluster verb answers and the process returns. The seam is threaded through
        // anyway, so the day one of these is asked from a running worker it reads the
        // live secret rather than the one this process was started with.
        Node::ConfiguredCredential const credential { cfg, nullptr };
        auto const answer = RunClusterAdmin(cfg, cfg.cluster, credential);
        if (!answer.has_value())
        {
            std::cerr << "fastcache-compile-node: " << answer.error() << '\n';
            return ExitUsage;
        }
        std::cout << *answer;
        return ExitOk;
    }

    // NOW the sink is chosen. Everything from here is what a RUNNING service reports
    // -- every startup refusal below, the toolchain survey, and the loop itself --
    // and a service has no console for any of it to land on (#179). Everything above
    // was an operator at a terminal, and stays there.
    //
    // The factory answers nullptr wherever there is no event log, which is what keeps
    // this one expression rather than a platform branch, and `cfg.daemon` rather than
    // "am I on Windows" is what keeps a foreground run pointed at its terminal on a
    // machine that has one.
    auto const eventLogger = cfg.daemon ? MakeWindowsEventLogger(cfg.serviceName, cfg.logLevel) : nullptr;
    ILogger& logger = eventLogger ? static_cast<ILogger&>(*eventLogger) : static_cast<ILogger&>(*consoleLogger);

    // Its OWN version, first thing, the way `fastcached` already does
    // ("fastcached {} starting"). A manually bundled install is built once,
    // deployed once, and then runs indefinitely with no further contact with what
    // is current -- and the persisted disk tier makes that worse rather than
    // better, because a stale object does not expire on its own. Without this line
    // "how old is this install" is answerable only by cross-referencing file
    // timestamps against git history by hand (#181).
    //
    // A LINE OF ITS OWN, deliberately, and not a field on the readiness line. That
    // line is a wire contract: four fixtures in two languages match its bytes, and
    // one of them `sed`s a field out of it by position. Adding a field there would
    // break them by TIMEOUT rather than by a failed build, which is the failure
    // mode #654 records about that exact string.
    logger.Logf(LogLevel::Info, "fastcache-compile-node {} starting", VersionString);

    // An empty `--scheduler` and "no --toolchain and no discovery" were both refused
    // HERE, as inline `if`s, and both are now `StartupPolicyRejection` rows -- the
    // toolchain pair by #403, the scheduler by #386. Neither reads anything but the
    // parsed configuration, which is what that table is for, and a rule in this tier
    // is one that a RELOAD cannot consult and that no test can reach: this file is in
    // no test target, so the copy that ran at startup was the untested one while the
    // tests asserted `NodeServiceRejection`'s install-time twin. They had already
    // drifted on the sentence they say.
    //
    // Nothing replaces them here. The table is consulted a few lines below, on the
    // same logger and with the same exit code, and there is no `return` in between --
    // so this is a deletion rather than a move of the refusal's timing.

    // Checked here rather than inside WorkerBody, for the reason the two above are:
    // the POSIX host has already redirected stdout to /dev/null by the time the body
    // runs, so a diagnosis printed there goes nowhere in the one deployment where a
    // scheduler is most likely to be misconfigured.
    if (auto const rejection = StartupPolicyRejection(cfg))
    {
        logger.Logf(LogLevel::Error, "{}", *rejection);
        return ExitUsage;
    }

    // **This node's identity, resolved once and applied to every configuration this
    // process builds** (#1024). It is minted into `--cluster-dir` on the first start
    // and read back on every one after, so a fleet is built without inventing a name
    // per machine and typing it twice.
    //
    // **AFTER the table that judges this command line, and that is not a preference.**
    // Resolving MINTS: it creates a directory and writes a file. A configuration the
    // node is about to refuse must not leave one behind -- and worse, an unwritable
    // `--cluster-dir` would answer with "cannot create ..." in place of the
    // configuration error that is the operator's actual problem. Which is also what
    // keeps the startup rules pure functions of the command line, since they never see
    // an identity: the self-peer row asks `--raft-self` rather than looking for the
    // member `ApplyNodeIdentity` synthesises.
    //
    // The install path resolves at its OWN site, after its own table, for the same
    // reason and it cannot share this one: `--install-service` returns long before
    // here, and its refusals are `NodeInstallRejection`'s rather than these.
    SystemRandomSource identityRandom;
    auto const identity = AdoptNodeIdentity(cfg, cliOnly, identityRandom, logger);
    if (!identity.has_value())
    {
        logger.Logf(LogLevel::Error, "{}; refusing to start", identity.error());
        return ExitUsage;
    }

    // Built only when there IS a file, and holding the SAME argv the startup parse
    // used -- so a reload reproduces the startup order exactly: a fresh configuration,
    // the file applied through the appliers argv reaches, then the command line second.
    // "The command line wins" therefore stays a question of which loop ran last, never
    // a per-field merge with per-field presence bits.
    //
    // **Declared before the secret-exposure watch below and before the daemon host**,
    // which is what makes both safe. The watch subscribes to this object and its
    // report closure refers to `logger`, declared further up, so the reloader is
    // destroyed first -- the ordering `WatchSecretExposure` requires. And the host is
    // chosen afterwards because the POSIX one redirects stdout to /dev/null inside
    // `Run()`: every startup warning has to be emitted before that.
    std::optional<NodeReloader> reloader;
    if (!lookup.path.empty())
        reloader.emplace(
            cfg,
            lookup.path,
            [argvSpan, identity = identity->value_or(Node::NodeIdentity {})](
                std::filesystem::path const& path) -> std::expected<NodeConfig, ConfigError> {
                // A FRESH configuration, never the live one. A file that fails halfway
                // is then discarded whole rather than leaving the running worker
                // holding part of a document nobody wrote.
                NodeConfig candidate;
                auto const loaded =
                    ReadYamlSettings(path).and_then([&candidate, &path, argvSpan](std::vector<YamlSetting> const& settings) {
                        return ApplyNodeConfiguration(settings, path, argvSpan.subspan(1), candidate);
                    });
                if (!loaded.has_value())
                    return std::unexpected(loaded.error());
                // The identity again, through the same function the start used. A
                // candidate rebuilt without it holds an empty `--node-id`, which is an
                // unreloadable field that has CHANGED -- so every reload would be
                // refused by name, on a worker whose configuration was perfectly
                // valid. Resolved once at startup and applied here, never re-resolved:
                // this runs on a signal, and a reload must not be able to mint.
                Node::ApplyNodeIdentity(candidate, identity);
                return candidate;
            },
            &ValidateNodeReloadable);

    // A secret is only as private as the file holding it, and this worker has FIVE
    // such files where the daemon has one. #384 landed that rule and wired only the
    // daemon; the exposure here is identical for `--requirepass` out of a
    // configuration file, and WIDER for the four secrets reached by path -- a
    // world-readable `--cluster-key-file` is the PSK that MACs both discovery proofs
    // and lease tokens, handed to every account on the machine
    // ([#752](https://github.com/LASTRADA-Software/fastcached/issues/752)).
    //
    // **And re-asked at every accepted reload**, which is the half a snapshot cannot
    // answer: a file's MODE is in no configuration, so an operator who loosens
    // `--cluster-key-file` an hour after this worker started was met with silence for
    // the rest of the process's life
    // ([#868](https://github.com/LASTRADA-Software/fastcached/issues/868)).
    //
    // `lookup.path` UNGUARDED by `fileApplied`, and never `cfg.configPath`. The
    // resolved path is what was opened, and a discovered machine-wide file is named by
    // no flag at all -- so the flag's value would answer "no file" for the deployment
    // the packaging ships. The `fileApplied` guard it used to carry is DROPPED because
    // it can never decide anything, and that is the whole claim -- MEASURED against the
    // control flow above rather than reasoned from what a reload might do. A non-empty
    // `lookup.path` that would not load already exited `ExitUsage` far above, except
    // under `fileIsAdvisory`, which is `--uninstall-service` and returns at the service
    // block. So every run that reaches this line with a path either applied the file or
    // never had one. Even in the unreachable case the gate answers the same: `cfg` IS
    // `cliOnly` there, so either argv named the token (`namedOnCommandLine`) or no
    // secret is in force (`secretInForce`).
    //
    // What it is NOT is a fix for a reload hole, and the reason CHANGED with #404
    // while the conclusion did not. `--requirepass` used to be `Reloadable::No`, so a
    // SIGHUP that introduced a token was refused and a node file could not GAIN a
    // secret across a reload the way the daemon's could; it is `Reloadable::Yes` now,
    // so it can. That makes the FILESYSTEM re-ask below load-bearing rather than
    // merely tidy -- and the re-ask already covers it, because `secretFiles` is a
    // function of the LIVE snapshot rather than of the configuration this process
    // started with. What this expression could never decide is still what it could
    // never decide: `argvNamedSecret` is about ARGV, which no reload rewrites.
    //
    // **A warning and not a refusal**, for #384's reason: refusing a MISSING
    // credential fails closed and breaks nothing that worked, while refusing an
    // EXPOSED one breaks a deployment that is running today. It goes to `logger`,
    // which is journald, the Windows event log or launchd's file -- a durable record
    // rather than a console line that scrolls past.
    //
    // After `StartupPolicyRejection`, so a worker that will not start is told the
    // one thing it can act on rather than that plus a list about files it never
    // reaches.
    //
    // **`!cliOnly.token.empty()` is provenance, not a value comparison.** `cliOnly` is
    // the command line ALONE -- parsed above, before any file was opened -- so a
    // non-empty token in it is argv having named one. The rulebook's clause is that a
    // flag whose default is EMPTY needs no explicit bit, there being nothing to arrive
    // at without asking; `--requirepass=` typed on purpose empties the merged value
    // too, and `secretInForce` is what answers it.
    //
    // A `tokenExplicit` column would buy nothing here and would be a live hazard.
    // `ApplyFileSettings` sets a row's `explicitBit` for a key it read out of the FILE
    // (`Config/FileOptions.hpp` says so, and names this call site), and the worker
    // applies the file and argv into ONE `NodeConfig` -- so the bit read off `cfg`
    // means "named anywhere" and answers true for exactly the secret this warning
    // exists to report. Only a bit read off `cliOnly` would be right, which is the
    // same fact this expression already reads, one column and one bool later.
    SecretSubjectFiles<NodeConfig> const secretFiles = [configFile = lookup.path,
                                                        argvNamedSecret = !cliOnly.token.empty()](NodeConfig const& live) {
        return NodeSecretFiles(live, configFile, argvNamedSecret);
    };
    SecretExposureReport report = [&logger](std::string_view warning) {
        logger.Logf(LogLevel::Warn, "{}", warning);
    };

    // Two arms, and the difference is whether there is a SECOND moment at all. A
    // worker configured entirely from argv still names four key files and still has to
    // be told about them; what it has no use for is the memory that keeps a standing
    // exposure from being repeated, because nothing will re-ask.
    if (reloader.has_value())
        WatchSecretExposure<NodeConfig>(*reloader, secretFiles, std::move(report));
    else
        ReportSecretExposure<NodeConfig>(cfg, secretFiles, report);

    // The host is chosen last, so everything that can be reported to a terminal
    // already has been. `--daemon` is what a SUPERVISOR THAT WANTS BACKGROUNDING
    // passes: the Windows SCM needs it, and systemd and launchd must not pass it,
    // because they supervise the process they started and reap a job that forks
    // as "exited".
    std::unique_ptr<IDaemonHost> host;
    if (cfg.daemon)
    {
#if defined(_WIN32)
        host = MakeWindowsServiceHost(cfg.serviceName);
#else
        // **A worker states its own working directory, and it is not `/`** (#784).
        //
        // Daemonizing has to leave the invocation directory, and `/` was what this
        // host chdir'd to unconditionally. For a process that spawns a compiler that
        // is a wrong object under a correct key: the worker builds
        // `-fdebug-prefix-map=<its own directory>=<what the client asked for>`, a
        // prefix-map rule appends the unmatched tail, and `/` matches every absolute
        // path in the object -- `/usr/include/stdio.h` becomes `.usr/include/stdio.h`.
        // #674 gave the shipped unit a `WorkingDirectory=`, which closes the packaged
        // route and not this one: a unit's directory does not survive the double
        // fork's chdir, because the chdir happens after it.
        //
        // `WorkerPrefixMapRules` still DROPS the worker's own rule when its directory
        // contains the client's, so a `--daemon` worker produced the pre-#506 clang
        // state rather than the corrupted object. That drop stays -- it is what covers
        // a hand-written unit -- and this makes the case it covers rarer rather than
        // relying on it.
        //
        // Created here rather than left to the host, because this is the last point
        // at which anything can be REPORTED: `PosixDaemonHost` has redirected stdout
        // to /dev/null by the time the body runs, so a failure diagnosed inside it
        // reaches nobody. A directory that cannot be made falls back to `/`, which is
        // what this call did before, and says so.
        std::error_code scratchBaseError;
        auto const daemonDirectory = Node::ScratchBaseDirectory();
        std::filesystem::create_directories(daemonDirectory, scratchBaseError);
        if (scratchBaseError)
            logger.Logf(LogLevel::Warn,
                        "cannot create {}: {}; daemonizing into / instead, where this worker's own "
                        "-fdebug-prefix-map rule is dropped and a dispatched clang object records this machine's "
                        "directory",
                        daemonDirectory.string(),
                        scratchBaseError.message());
        auto const daemonWorkingDirectory = scratchBaseError ? std::filesystem::path { "/" } : daemonDirectory;
        host = MakePosixDaemonHost(cfg.pidfile, daemonWorkingDirectory.string());
#endif
    }
    if (!host)
        host = std::make_unique<ForegroundHost>();

    auto* const reloaderPtr = reloader.has_value() ? &*reloader : nullptr;
    return host->Run([&cfg, &logger, reloaderPtr] { return WorkerBody(cfg, logger, reloaderPtr); });
}
